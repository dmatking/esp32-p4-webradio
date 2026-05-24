#include "slave_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "slave_ota";

#define CHUNK_SIZE 1500
#define SLAVE_FW_PARTITION "slave_fw"

esp_err_t slave_ota_update_if_needed(void)
{
    // Get current co-processor firmware version
    esp_hosted_coprocessor_fwver_t ver = {0};
    esp_err_t ret = esp_hosted_get_coprocessor_fwversion(&ver);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot get co-processor version: %s", esp_err_to_name(ret));
        // Proceed anyway -- old firmware may not support the version query
    } else {
        ESP_LOGI(TAG, "Co-processor firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
    }

    // Find the slave firmware partition
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, SLAVE_FW_PARTITION);
    if (!part) {
        ESP_LOGW(TAG, "No '%s' partition found, skipping OTA", SLAVE_FW_PARTITION);
        return ESP_OK;
    }

    // Check if partition has data (not all 0xFF)
    uint8_t hdr[4];
    ret = esp_partition_read(part, 0, hdr, sizeof(hdr));
    if (ret != ESP_OK || (hdr[0] == 0xFF && hdr[1] == 0xFF && hdr[2] == 0xFF && hdr[3] == 0xFF)) {
        ESP_LOGW(TAG, "Partition '%s' appears empty, skipping OTA", SLAVE_FW_PARTITION);
        return ESP_OK;
    }

    // Read image header to get firmware size
    esp_image_header_t img_hdr;
    ret = esp_partition_read(part, 0, &img_hdr, sizeof(img_hdr));
    if (ret != ESP_OK || img_hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGW(TAG, "Invalid firmware image in '%s', skipping OTA", SLAVE_FW_PARTITION);
        return ESP_OK;
    }

    // Read app descriptor to get version string
    char new_ver[32] = "unknown";
    if (img_hdr.segment_count > 0) {
        esp_app_desc_t app_desc;
        size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
        if (esp_partition_read(part, desc_off, &app_desc, sizeof(app_desc)) == ESP_OK) {
            strncpy(new_ver, app_desc.version, sizeof(new_ver) - 1);
            new_ver[sizeof(new_ver) - 1] = '\0';
        }
    }

    // Check if versions match
    if (ret == ESP_OK && ver.major1 != 0) {
        char cur_ver[32];
        snprintf(cur_ver, sizeof(cur_ver), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
        if (strcmp(cur_ver, new_ver) == 0) {
            ESP_LOGI(TAG, "Co-processor already at %s, no update needed", cur_ver);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Updating co-processor: %s -> %s", cur_ver, new_ver);
    } else {
        ESP_LOGI(TAG, "Updating co-processor to %s", new_ver);
    }

    // Calculate total firmware size by walking segments
    size_t fw_size = sizeof(esp_image_header_t);
    size_t offset = sizeof(esp_image_header_t);
    for (int i = 0; i < img_hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        ret = esp_partition_read(part, offset, &seg, sizeof(seg));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read segment %d", i);
            return ESP_FAIL;
        }
        fw_size += sizeof(seg) + seg.data_len;
        offset += sizeof(seg) + seg.data_len;
    }
    // Padding to 16-byte alignment + checksum byte
    size_t pad = (16 - (fw_size % 16)) % 16;
    fw_size += pad + 1;
    // SHA256 hash if appended
    if (img_hdr.hash_appended)
        fw_size += 32;

    ESP_LOGI(TAG, "Firmware size: %u bytes", (unsigned)fw_size);

    // Perform OTA
    ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t chunk[CHUNK_SIZE];
    offset = 0;
    while (offset < fw_size) {
        size_t to_read = (fw_size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (fw_size - offset);
        ret = esp_partition_read(part, offset, chunk, to_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Partition read failed at offset %u", (unsigned)offset);
            esp_hosted_slave_ota_end();
            return ret;
        }
        ret = esp_hosted_slave_ota_write(chunk, to_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at offset %u", (unsigned)offset);
            esp_hosted_slave_ota_end();
            return ret;
        }
        offset += to_read;
        if ((offset * 100 / fw_size) % 25 == 0 && offset > 0) {
            ESP_LOGI(TAG, "OTA progress: %u%%", (unsigned)(offset * 100 / fw_size));
        }
    }

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "OTA complete, activating new firmware...");
    ret = esp_hosted_slave_ota_activate();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA activate returned %s (may not be supported on old FW)", esp_err_to_name(ret));
        // Old firmware may not support activate -- the OTA end should suffice
    }

    // The C6 will reboot. Give it time to come back up.
    ESP_LOGI(TAG, "Co-processor rebooting with new firmware...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Re-establish hosted connection
    ESP_LOGI(TAG, "Reconnecting to co-processor...");
    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconnect after OTA: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Co-processor updated successfully!");
    return ESP_OK;
}

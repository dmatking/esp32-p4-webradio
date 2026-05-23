// Copyright 2026 David M. King
// SPDX-License-Identifier: MIT

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "captive_portal.h"

#include "wifi_prov.h"

#define TAG    "wifi_prov"
#define NVS_NS "wifi_prov"

#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
#define SAVED_BIT     BIT2

static EventGroupHandle_t        s_eg;
static int                       s_retry;
static const wifi_prov_config_t *s_cfg;
static bool                      s_force_portal;
static bool                      s_connect_failed;

// --- URL decode -------------------------------------------------------

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 1) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// Extract value for key= from a URL-encoded body string.
static bool form_get(const char *body, const char *key,
                     char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    char raw[256] = { 0 };
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(out, raw, out_len);
    return out[0] != '\0';
}

// --- WiFi STA event handler -------------------------------------------

static void sta_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < 5) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGI(TAG, "Retry %d/5...", s_retry);
        } else {
            xEventGroupSetBits(s_eg, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected — " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_eg, CONNECTED_BIT);
    }
}

// --- STA connect ------------------------------------------------------

static bool sta_connect(const char *ssid, const char *pass)
{
    s_retry = 0;
    xEventGroupClearBits(s_eg, CONNECTED_BIT | FAIL_BIT);

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, sta_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event_handler, NULL, &inst_got_ip));

    wifi_config_t wcfg = { 0 };
    strncpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode =
        (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_eg,
        CONNECTED_BIT | FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    if (bits & CONNECTED_BIT) return true;

    // Clean up so the portal can reinitialize WiFi for another attempt
    ESP_LOGE(TAG, "Failed to connect to '%s'", ssid);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_any);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_got_ip);
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(sta_netif);
    return false;
}

// --- HTTP handlers ----------------------------------------------------

static esp_err_t handle_form_get(httpd_req_t *req)
{
    const size_t BUF = 8192;
    char *html = malloc(BUF);
    if (!html) return ESP_ERR_NO_MEM;

    // Pre-populate SSID from NVS (password intentionally left blank)
    char saved_ssid[64] = { 0 };
    nvs_handle_t nvs_r;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs_r) == ESP_OK) {
        size_t len = sizeof(saved_ssid);
        nvs_get_str(nvs_r, "ssid", saved_ssid, &len);
        nvs_close(nvs_r);
    }

    const char *err_banner = s_connect_failed
        ? "<p style='color:#c00;background:#fee;padding:8px;border-radius:4px'>"
          "&#9888; Connection failed &mdash; check your WiFi password and try again."
          "</p>"
        : "";

    int n = snprintf(html, BUF,
        "<!DOCTYPE html><html><head><title>Device Setup</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>"
        "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px}"
        "label{display:block;margin:12px 0 4px}"
        "input,select{width:100%%;padding:8px;box-sizing:border-box;"
        "border:1px solid #ccc;border-radius:4px;font-size:15px}"
        "button{margin-top:24px;width:100%%;padding:12px;background:#0070f3;"
        "color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer}"
        "</style></head><body>"
        "<h2>Device Setup</h2>"
        "%s"
        "<p>Enter your WiFi credentials to connect this device to your network.</p>"
        "<form method='POST' action='/save'>"
        "<label>WiFi Network (SSID)</label>"
        "<input name='ssid' autocomplete='off' placeholder='Network name' value='%s'>"
        "<label>WiFi Password</label>"
        "<input type='password' name='pass' placeholder='%s'>",
        err_banner,
        saved_ssid,
        saved_ssid[0] ? "Leave blank to keep current password" : "Password (leave blank if open)");

    if (s_cfg && s_cfg->extra_fields) {
        bool nvs_ok = (nvs_open(NVS_NS, NVS_READONLY, &nvs_r) == ESP_OK);

        for (int i = 0; i < s_cfg->extra_count; i++) {
            const wifi_prov_field_t *f = &s_cfg->extra_fields[i];
            if (BUF - n < 512) break;

            // Read saved value for non-secret fields
            char saved_val[256] = { 0 };
            if (nvs_ok && !f->secret) {
                size_t vlen = sizeof(saved_val);
                nvs_get_str(nvs_r, f->key, saved_val, &vlen);
            }

            if (f->options && f->option_count > 0) {
                n += snprintf(html + n, BUF - n,
                    "<label>%s</label><select name='%s'>",
                    f->label, f->key);
                for (int j = 0; j < f->option_count && BUF - n > 128; j++) {
                    bool sel = saved_val[0] &&
                               strcmp(saved_val, f->options[j].value) == 0;
                    n += snprintf(html + n, BUF - n,
                        "<option value='%s'%s>%s</option>",
                        f->options[j].value, sel ? " selected" : "",
                        f->options[j].label);
                }
                n += snprintf(html + n, BUF - n, "</select>");
            } else if (f->input_type) {
                char min_attr[24] = "", max_attr[24] = "";
                if (f->input_min) snprintf(min_attr, sizeof(min_attr), " min='%s'", f->input_min);
                if (f->input_max) snprintf(max_attr, sizeof(max_attr), " max='%s'", f->input_max);
                n += snprintf(html + n, BUF - n,
                    "<label>%s</label>"
                    "<input type='%s'%s%s name='%s' placeholder='%s' value='%s'>",
                    f->label, f->input_type, min_attr, max_attr,
                    f->key, f->placeholder ? f->placeholder : "",
                    saved_val);
            } else if (f->secret) {
                n += snprintf(html + n, BUF - n,
                    "<label>%s</label>"
                    "<input type='password' name='%s' placeholder='%s'>",
                    f->label, f->key,
                    f->placeholder ? f->placeholder : "");
            } else {
                n += snprintf(html + n, BUF - n,
                    "<label>%s</label>"
                    "<input name='%s' placeholder='%s' value='%s'>",
                    f->label, f->key,
                    f->placeholder ? f->placeholder : "",
                    saved_val);
            }
        }
        if (nvs_ok) nvs_close(nvs_r);
    }

    n += snprintf(html + n, BUF - n,
        "<button type='submit'>Save &amp; Connect</button>"
        "</form></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, n);
    free(html);
    return ESP_OK;
}

static esp_err_t handle_form_post(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char *body = malloc(len + 1);
    if (!body) return ESP_ERR_NO_MEM;

    int recv = httpd_req_recv(req, body, len);
    if (recv <= 0) { free(body); return ESP_FAIL; }
    body[recv] = '\0';

    char ssid[64] = { 0 };
    char pass[128] = { 0 };
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));

    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        free(body);
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        if (pass[0]) nvs_set_str(nvs, "pass", pass);

        if (s_cfg && s_cfg->extra_fields) {
            for (int i = 0; i < s_cfg->extra_count; i++) {
                const wifi_prov_field_t *f = &s_cfg->extra_fields[i];
                char val[256] = { 0 };
                if (form_get(body, f->key, val, sizeof(val)))
                    nvs_set_str(nvs, f->key, val);
            }
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    free(body);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:sans-serif;"
        "max-width:400px;margin:40px auto;padding:20px'>"
        "<h2>&#10003; Saved</h2>"
        "<p>Credentials saved. The device will now connect to your WiFi. "
        "You can close this page.</p>"
        "</body></html>");

    xEventGroupSetBits(s_eg, SAVED_BIT);
    return ESP_OK;
}

// --- Provisioning portal ----------------------------------------------

static void run_portal(void)
{
    xEventGroupClearBits(s_eg, SAVED_BIT);  // clear any stale bit from a previous run

    const char *ap_ssid = (s_cfg && s_cfg->ap_ssid) ? s_cfg->ap_ssid : "ESP32-Config";
    const char *ap_pass = s_cfg ? s_cfg->ap_password : NULL;

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len      = strlen(ap_ssid);
    ap_cfg.ap.max_connection = 4;
    if (ap_pass && ap_pass[0]) {
        strncpy((char *)ap_cfg.ap.password, ap_pass,
                sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_cfg && s_cfg->on_portal)
        s_cfg->on_portal(ap_ssid, s_cfg->on_portal_ctx);

    ESP_LOGI(TAG, "Provisioning AP started: '%s'", ap_ssid);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.uri_match_fn    = httpd_uri_match_wildcard;
    http_cfg.max_uri_handlers = 16;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t get_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_form_get,
    };
    httpd_uri_t post_save = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = handle_form_post,
    };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_save);
    captive_portal_register(server, NULL);

    // Block until credentials submitted
    xEventGroupWaitBits(s_eg, SAVED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Credentials received, tearing down AP");

    vTaskDelay(pdMS_TO_TICKS(500));  // let success page finish sending

    httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(ap_netif);
}

// --- Public API -------------------------------------------------------

bool wifi_prov_start(const wifi_prov_config_t *cfg)
{
    s_cfg = cfg;
    s_eg  = xEventGroupCreate();

    // NVS init
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    // netif + event loop — ignore already-initialized errors
    esp_netif_init();
    esp_event_loop_create_default();

    // Boot GPIO: hold > 3 s to wipe credentials
    int boot_gpio = cfg ? cfg->boot_gpio : -1;
    if (boot_gpio >= 0) {
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << boot_gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&gc);
        if (gpio_get_level(boot_gpio) == 0) {
            ESP_LOGI(TAG, "Boot GPIO held — waiting 3 s to confirm re-provision...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            if (gpio_get_level(boot_gpio) == 0) {
                ESP_LOGI(TAG, "Forcing provisioning portal (settings preserved)");
                s_force_portal = true;
            }
        }
    }
    if (cfg && cfg->force_portal) {
        ESP_LOGI(TAG, "force_portal requested by caller");
        s_force_portal = true;
    }

    // Try to load stored credentials
    char ssid[64] = { 0 };
    char pass[128] = { 0 };
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        size_t slen = sizeof(ssid), plen = sizeof(pass);
        nvs_get_str(nvs, "ssid", ssid, &slen);
        nvs_get_str(nvs, "pass", pass, &plen);
        nvs_close(nvs);
    }

    const char *ap_ssid = (cfg && cfg->ap_ssid) ? cfg->ap_ssid : "ESP32-Config";

    if (ssid[0] && !s_force_portal) {
        ESP_LOGI(TAG, "Stored SSID found: '%s'", ssid);
        if (sta_connect(ssid, pass)) return true;
        ESP_LOGW(TAG, "Stored credentials failed — falling back to portal");
        s_connect_failed = true;
        if (s_cfg && s_cfg->on_connect_failed)
            s_cfg->on_connect_failed(ap_ssid, s_cfg->on_portal_ctx);
    }

    // Portal loop: show form, try connect, retry if failed
    while (1) {
        ESP_LOGI(TAG, "Starting provisioning portal");
        run_portal();

        memset(ssid, 0, sizeof(ssid));
        memset(pass, 0, sizeof(pass));
        if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
            size_t slen = sizeof(ssid), plen = sizeof(pass);
            nvs_get_str(nvs, "ssid", ssid, &slen);
            nvs_get_str(nvs, "pass", pass, &plen);
            nvs_close(nvs);
        }

        if (sta_connect(ssid, pass)) return true;
        ESP_LOGW(TAG, "Connection failed — re-opening portal for another attempt");
        s_connect_failed = true;
        if (s_cfg && s_cfg->on_connect_failed)
            s_cfg->on_connect_failed(ap_ssid, s_cfg->on_portal_ctx);
    }
}

bool wifi_prov_get(const char *key, char *buf, size_t len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(nvs, key, buf, &len);
    nvs_close(nvs);
    return err == ESP_OK;
}

void wifi_prov_reset(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Credentials erased");
    }
}

// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// WiFi bring-up via the wifi_prov component: stored NVS credentials, or a
// captive-portal SoftAP for first-boot setup. No credentials are baked into
// the firmware. WiFi hardware is the ESP32-C6 co-processor over esp-hosted.

#include <stdbool.h>
#include "esp_log.h"
#include "esp_hosted.h"
#include "wifi_prov.h"
#include "wifi.h"

#define TAG "wifi"

// SoftAP name the user joins to provision the radio.
#define PROV_AP_SSID "P4-Radio-Setup"

static wifi_portal_cb_t s_on_portal;

// wifi_prov passes a void* ctx we don't need; adapt to our simpler callback.
static void portal_started(const char *ap_ssid, void *ctx)
{
    if (s_on_portal) s_on_portal(ap_ssid);
}

bool wifi_connect(wifi_portal_cb_t on_portal)
{
    s_on_portal = on_portal;

    // Initialize the ESP32-C6 co-processor (provides WiFi via SDIO) before
    // any WiFi calls — wifi_prov handles NVS/netif/event-loop itself.
    ESP_LOGI(TAG, "Initializing co-processor...");
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Co-processor ready");

    wifi_prov_config_t cfg = {
        .ap_ssid           = PROV_AP_SSID,
        .ap_password       = NULL,   // open AP for setup
        .boot_gpio         = -1,     // no physical reprovision button
        .on_portal         = portal_started,
        .on_connect_failed = portal_started,
    };
    return wifi_prov_start(&cfg);
}

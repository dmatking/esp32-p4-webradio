// Copyright 2026 David M. King
// SPDX-License-Identifier: MIT
//
// WiFi provisioning component: captive-portal first-boot setup with NVS
// credential storage.  On first boot (or after wipe) the device starts a
// SoftAP + captive portal so the user can enter credentials via a web form.
// Credentials are stored in NVS namespace "wifi_prov" and reused on
// subsequent boots.

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One choice in a dropdown field.
typedef struct {
    const char *label;   // displayed text
    const char *value;   // submitted value
} wifi_prov_option_t;

// Describes one extra form field beyond SSID/password.
// The key is used as both the HTML input name and the NVS key.
//
// Rendering rules (checked in order):
//   options != NULL              → <select> dropdown
//   input_type != NULL           → <input type=input_type min max>
//   secret == true               → <input type="password">
//   otherwise                    → <input type="text">
typedef struct {
    const char *key;          // NVS key (max 15 chars)
    const char *label;        // HTML label text
    const char *placeholder;  // HTML placeholder or default value, NULL = none

    bool        secret;       // render as type="password" (text fields only)

    // Dropdown — if set, renders a <select> and ignores secret/input_type.
    const wifi_prov_option_t *options;
    int                       option_count;

    // Number / custom input type — ignored when options is set.
    const char *input_type;   // e.g. "number"; NULL = "text"
    const char *input_min;    // HTML min attribute, NULL = omit
    const char *input_max;    // HTML max attribute, NULL = omit
} wifi_prov_field_t;

// Called when the provisioning portal starts (device can update its display).
typedef void (*wifi_prov_display_fn_t)(const char *ap_ssid, void *ctx);

typedef struct {
    // SoftAP name shown during provisioning.  NULL defaults to "ESP32-Config".
    const char *ap_ssid;

    // SoftAP password.  NULL or empty string = open (no password) AP.
    const char *ap_password;

    // GPIO number to hold at boot to force re-provisioning (hold > 3 s).
    // -1 disables this feature.
    int boot_gpio;

    // Called just before the portal becomes active.  NULL = no callback.
    wifi_prov_display_fn_t on_portal;

    // Called when a connection attempt fails, before the portal restarts.
    // ap_ssid is the SoftAP name the user should reconnect to.  NULL = no callback.
    wifi_prov_display_fn_t on_connect_failed;

    void                  *on_portal_ctx;

    // Optional extra fields appended to the provisioning form.
    const wifi_prov_field_t *extra_fields;
    int                      extra_count;

    // If true, skip the stored-credential fast path and go straight to the
    // captive portal.  Saved values are preserved (non-secret fields
    // pre-populate the form).  Used to recover from app-level auth failures
    // without wiping WiFi creds.
    bool force_portal;
} wifi_prov_config_t;

// Connect to WiFi using stored credentials, or run the captive-portal
// provisioning flow if no credentials are stored.  Blocks until connected.
// Returns true on success, false if the connection fails.
//
// The caller is responsible for any chip-specific WiFi hardware init
// (e.g. esp_hosted_init() on ESP32-P4) before calling this function.
bool wifi_prov_start(const wifi_prov_config_t *cfg);

// Read a value from NVS by key ("ssid", "pass", or any extra field key).
// Returns false if the key is not found.
bool wifi_prov_get(const char *key, char *buf, size_t len);

// Erase all stored credentials, forcing provisioning on next boot.
void wifi_prov_reset(void);

#ifdef __cplusplus
}
#endif

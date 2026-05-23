// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Called when the captive-portal provisioning starts, so the UI can show
// setup instructions. `ap_ssid` is the SoftAP name the user should join.
typedef void (*wifi_portal_cb_t)(const char *ap_ssid);

// Bring up WiFi: connect with credentials stored in NVS, or run the
// captive-portal provisioning flow (SoftAP) if none are stored.
// Blocks until connected. `on_portal` (nullable) fires when the portal opens.
bool wifi_connect(wifi_portal_cb_t on_portal);

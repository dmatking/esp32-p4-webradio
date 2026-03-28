// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Connect to WiFi using credentials from Kconfig (loaded from ~/.esp_creds).
// Blocks until connected or fails (logs error and returns false).
bool wifi_connect(void);

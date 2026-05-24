#pragma once

#include "esp_err.h"

// Update C6 co-processor firmware from the slave_fw partition if needed.
// Call after esp_hosted_connect_to_slave() succeeds.
// Returns ESP_OK if update was performed or not needed.
esp_err_t slave_ota_update_if_needed(void);

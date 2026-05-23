idf_component_register(
    SRCS "wifi_prov.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        nvs_flash
        driver
        log
        freertos
    PRIV_REQUIRES
        nordesems__esp-captive-portal
)

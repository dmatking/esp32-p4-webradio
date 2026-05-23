cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS
        "captive_portal.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        freertos
        log
        lwip
)

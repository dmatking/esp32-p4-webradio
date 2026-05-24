include("${CMAKE_CURRENT_LIST_DIR}/../board_config.cmake")
message(STATUS "COMPONENT: BOARD='${BOARD}'")

set(BOARD_SRC "")
set(EXTRA_REQUIRES "")

if(BOARD STREQUAL "tab5")
    set(BOARD_SRC "board_m5stack_tab5.c")
    list(APPEND EXTRA_REQUIRES esp_codec_dev esp_lcd_touch_st7123 esp_lcd_st7123 esp_driver_ppa)
else()
    set(BOARD_SRC "board_waveshare_p4_720.c")
    list(APPEND EXTRA_REQUIRES esp_lcd_st7703 es8311)
endif()

idf_component_register(
    SRCS
        "main.c"
        ${BOARD_SRC}
        "wifi.c"
        "radio_browser.c"
        "stream.c"
        "mp3_decoder.c"
        "spectrum.c"
        "font8x16.c"
        "touch.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_psram
        esp_mm
        esp_timer
        log
        driver
        esp_driver_i2s
        esp_wifi
        esp_event
        nvs_flash
        esp_netif
        esp_http_client
        esp-tls
        json
        esp_hosted
        wifi_prov
        ${EXTRA_REQUIRES}
)

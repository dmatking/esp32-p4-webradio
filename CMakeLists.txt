cmake_minimum_required(VERSION 3.16)

# Explicitly include project defaults, then layer in local credentials.
# Must be done before including project.cmake — once SDKCONFIG_DEFAULTS is
# set to any value, ESP-IDF stops auto-detecting sdkconfig.defaults.
list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "$ENV{HOME}/.esp_creds")
    list(APPEND SDKCONFIG_DEFAULTS "$ENV{HOME}/.esp_creds")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_radio)

cmake_minimum_required(VERSION 3.16)

# WiFi credentials are provisioned at runtime (captive portal -> NVS), so no
# credential file is layered into the build. sdkconfig.defaults is set
# explicitly because once SDKCONFIG_DEFAULTS has any value, ESP-IDF stops
# auto-detecting it.
list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_radio)

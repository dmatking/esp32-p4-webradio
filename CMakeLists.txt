cmake_minimum_required(VERSION 3.16)

# Board selection: BOARD=tab5 idf build   (default: waveshare)
# Use an env var — cmake -D doesn't propagate into IDF component scopes.
if(DEFINED ENV{BOARD})
    set(BOARD "$ENV{BOARD}")
else()
    set(BOARD "waveshare")
endif()
message(STATUS "ROOT: BOARD='${BOARD}'")

# Persist the selection so the component CMakeLists (a separate cmake process)
# can read it.
file(WRITE "${CMAKE_SOURCE_DIR}/board_config.cmake" "set(BOARD \"${BOARD}\")\n")

# Swap in the board-specific component manifest before the IDF component
# manager reads it. idf_component.yml is generated — do not edit by hand.
if(BOARD STREQUAL "tab5")
    file(READ ${CMAKE_SOURCE_DIR}/main/idf_component.tab5.yml _manifest)
else()
    file(READ ${CMAKE_SOURCE_DIR}/main/idf_component.waveshare.yml _manifest)
endif()
file(WRITE ${CMAKE_SOURCE_DIR}/main/idf_component.yml "${_manifest}")

# WiFi credentials are provisioned at runtime (captive portal -> NVS), so no
# credential file is layered into the build. sdkconfig.defaults must be listed
# explicitly because once SDKCONFIG_DEFAULTS has any value IDF stops
# auto-detecting it. Board-specific overrides layer on top.
list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(BOARD STREQUAL "tab5")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.tab5")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_radio)

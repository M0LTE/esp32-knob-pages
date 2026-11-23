# The portable UI (shared with the desktop simulator) is compiled straight from ../ui.
# -DDESK_UI_DIR=... builds another copy of it (for trying out page changes).
if(NOT DESK_UI_DIR)
    set(DESK_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ui)
endif()
file(GLOB UI_SRCS ${DESK_UI_DIR}/*.c ${DESK_UI_DIR}/pages/*.c ${DESK_UI_DIR}/fonts/*.c)
idf_component_register(SRCS "main.c" "board.c" "audio.c" "haptic.c" "platform.c" "settings.c"
                            "net.c" "web.c" "setup_screen.c"
                            "src_ping.c" "src_kiss.c" "src_metrics.c" "src_energy.c" "src_rooms.c" "src_adsb.c" "src_waterfall.c"
                            "kiss_parse.c" "metrics_parse.c" "energy_parse.c" "rooms_parse.c" "adsb_parse.c" "ws_parse.c" "wf_fold.c" "rx_audio.c" "ota.c" "gpsdo_parse.c" "src_gpsdo.c" "src_door.c" "bench.c" "scope.c" "scope_stitch.c"
                            ${UI_SRCS}
                       INCLUDE_DIRS "." ${DESK_UI_DIR}
                       LDFRAGMENTS "linker.lf"
                       REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc
                                esp_lcd esp_timer esp_psram nvs_flash json esp_wifi esp_netif esp_event
                                esp_http_server esp_http_client esp-tls lwip mbedtls app_update espcoredump mqtt esp_app_format esp_phy heap)
# The JPEG decoder's working memory is steered to PSRAM (see src_door.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=heap_caps_calloc_prefer" "-Wl,--wrap=heap_caps_aligned_calloc")
# The UI is written for the simulator's warnings; keep the device build quiet about the same things.
set_source_files_properties(${UI_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable;-Wno-format-truncation")
if(ADSB_TEST_HOME)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ADSB_TEST_HOME=1)
endif()
# Optional build defaults: idf.py -DDESK_DEFAULTS=path/to/defaults.json build
# (same schema as the settings stored on the device). Secrets in it are ignored.
if(DESK_DEFAULTS)
    get_filename_component(_defaults "${DESK_DEFAULTS}" ABSOLUTE BASE_DIR "${PROJECT_DIR}")
    if(NOT EXISTS "${_defaults}")
        message(FATAL_ERROR "DESK_DEFAULTS: ${_defaults} not found")
    endif()
    file(READ "${_defaults}" _djson)
    foreach(_secret "\"pass\"" "\"admin_pass\"" "\"token\"" "\"home_lat\"" "\"home_lon\"")
        string(FIND "${_djson}" "${_secret}" _at)
        if(NOT _at EQUAL -1)
            message(WARNING "DESK_DEFAULTS contains ${_secret}: it will be ignored on the device. Keep secrets and the home location out of build defaults.")
        endif()
    endforeach()
    configure_file("${_defaults}" "${CMAKE_CURRENT_BINARY_DIR}/desk_defaults.json" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/desk_defaults.json" TEXT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DESK_HAS_DEFAULTS=1)
    message(STATUS "Desk build defaults: ${_defaults}")
endif()
if(WF_TEST_FIRST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WF_TEST_FIRST=1)
endif()
if(WF_TEST_LISTEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WF_TEST_LISTEN=1)
endif()
if(OTA_TEST_NEVER_HEALTHY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE OTA_TEST_NEVER_HEALTHY=1)
endif()
if(DESK_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DESK_BENCH=1)
endif()

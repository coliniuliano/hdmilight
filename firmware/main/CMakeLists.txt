set(hl_srcs "app_main.c" "capture.c" "leds.c" "config.c" "net.c" "web.c" "logbuf.c" "esphome_api.c" "homeassistant.c" "netwatch.c" "crashlog.c"
            "console.c")
# The oneboard build (revision A) also runs the on-board splitter and manages the capture chip.
if(CONFIG_HL_BOARD_ONEBOARD)
    list(APPEND hl_srcs "oneboard.c" "hdmi.c" "msflash.c" "cec.c" "cec_core.c" "ms2130_image.c" "ms213x_proto.c" "ms2130_usb.c")
endif()
idf_component_register(SRCS ${hl_srcs}
                       INCLUDE_DIRS "."
                       REQUIRES hl_core usb esp_driver_jpeg esp_driver_gpio esp_timer nvs_flash
                                esp_wifi esp_netif esp_event esp_http_server dns_server app_update esp_app_format
                                gsv esp_driver_i2c esp_driver_spi esp_driver_gptimer esp_driver_uart esp_driver_usb_serial_jtag
                       EMBED_TXTFILES "www/index.html")

# Tone-map LUT baked at build time with the host compiler (tools/bake_lut.c + core/), from
# model_calibrated.h when present. Regenerated whenever the model, the core or the size changes.
set(HL_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
set(HL_TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../tools)
file(GLOB HL_CORE_SRCS ${HL_CORE_DIR}/src/*.c)
file(GLOB HL_CORE_HDRS ${HL_CORE_DIR}/include/*.h)
set(HL_LUT_DEPS ${HL_TOOLS_DIR}/bake_lut.c ${HL_CORE_SRCS} ${HL_CORE_HDRS})
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/model_calibrated.h)
    list(APPEND HL_LUT_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/model_calibrated.h)
endif()
find_program(HL_HOST_CC NAMES cc gcc clang)
if(NOT HL_HOST_CC)
    message(FATAL_ERROR "A host C compiler (cc/gcc/clang) is needed to bake the tone-map LUT")
endif()
set(HL_LUT_BIN ${CMAKE_CURRENT_BINARY_DIR}/lut_baked.bin)
add_custom_command(OUTPUT ${HL_LUT_BIN}
    COMMAND ${HL_HOST_CC} -std=c11 -O2 -I${HL_CORE_DIR}/include -I${CMAKE_CURRENT_SOURCE_DIR}
            ${HL_TOOLS_DIR}/bake_lut.c ${HL_CORE_SRCS} -lm -o ${CMAKE_CURRENT_BINARY_DIR}/bake_lut
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/bake_lut ${CONFIG_HL_TONEMAP_LUT_N} ${HL_LUT_BIN}
    DEPENDS ${HL_LUT_DEPS}
    COMMENT "Baking the ${CONFIG_HL_TONEMAP_LUT_N}^3 tone-map LUT with the host compiler")
add_custom_target(hl_lut_bake DEPENDS ${HL_LUT_BIN})
add_dependencies(${COMPONENT_LIB} hl_lut_bake)
target_add_binary_data(${COMPONENT_LIB} ${HL_LUT_BIN} BINARY)

# Crash capture (crashlog.c): every panic goes through esp_panic_handler; wrap it to save the details first.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# The P4's Wi-Fi runs through esp_hosted, and stock 3.1.0 wedges SDIO RX for good with the C6 firmware the dev kits
# ship (patches/esp_hosted/README.md). Refuse to build the P4 image against anything but the patched copy.
idf_build_get_property(hl_idf_target IDF_TARGET)
if(hl_idf_target STREQUAL "esp32p4")
    idf_build_get_property(hl_components BUILD_COMPONENTS)
    set(hl_eh_dir "")
    if("esp_hosted" IN_LIST hl_components)
        idf_component_get_property(hl_eh_dir esp_hosted COMPONENT_DIR)
    endif()
    if("espressif__esp_hosted" IN_LIST hl_components OR NOT hl_eh_dir OR NOT EXISTS "${hl_eh_dir}/HDMILIGHT_PATCH.md")
        message(FATAL_ERROR "hdmilight: the P4 build must use the patched esp_hosted generated in firmware/.patched/esp_hosted "
                            "(override_path in main/idf_component.yml). Stock 3.1.0 wedges Wi-Fi RX on the dev kits' C6; "
                            "read patches/esp_hosted/README.md before changing it.")
    endif()
    message(STATUS "hdmilight: using patched esp_hosted at ${hl_eh_dir}")
endif()

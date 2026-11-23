cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)
# The captive-portal DNS server (main/net.c: while the setup AP is up it answers every name with 192.168.4.1, so
# joining the board's network pops the setup page by itself) is an ESP-IDF example component. Espressif has never
# published it to the component registry, but it ships inside the IDF, so take it from the IDF in use rather than
# keeping a copy of their code here. That also keeps it in step with whatever IDF version is building.
set(hl_dns_dir "$ENV{IDF_PATH}/examples/protocols/http_server/captive_portal/components")
if(NOT EXISTS "${hl_dns_dir}/dns_server/CMakeLists.txt")
    message(FATAL_ERROR "hdmilight: the dns_server example component is not at ${hl_dns_dir}. It ships with "
                        "ESP-IDF under examples/protocols/http_server/captive_portal; if this IDF moved it, "
                        "point hl_dns_dir in firmware/CMakeLists.txt at its new location.")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${hl_dns_dir}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One tracked lock file per chip, so component versions never change without a commit. The target is not known
# yet at this point: take it from the cache/environment (set-target), else from the sdkconfig in use.
set(hl_target "${IDF_TARGET}")
if(NOT hl_target)
    set(hl_target "$ENV{IDF_TARGET}")
endif()
if(NOT hl_target)
    if(SDKCONFIG)
        set(hl_sdkconfig "${SDKCONFIG}")
    else()
        set(hl_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    endif()
    if(EXISTS "${hl_sdkconfig}")
        file(STRINGS "${hl_sdkconfig}" hl_line REGEX "^CONFIG_IDF_TARGET=\"[a-z0-9]+\"$")
        string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"([a-z0-9]+)\"$" "\\1" hl_target "${hl_line}")
    endif()
endif()
if(NOT hl_target)
    message(FATAL_ERROR "hdmilight: cannot tell the target for the lock file; run idf.py set-target first")
endif()
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${hl_target}")
message(STATUS "hdmilight: dependency lock dependencies.lock.${hl_target}")
# Two components are a pinned registry release plus a patch from patches/, generated here so they exist before the
# component manager resolves override_path: the UVC host driver (bulk payload boundaries for the MS2130) and, on the
# P4, esp_hosted (SDIO RX). patches/<name>/README.md says why each one is needed.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/patched_component.cmake")
hl_patched_component(usb_host_uvc 2.6.0 148198b9610e1ca32c36ae396177050a9b8adba0f57ab46f5faa2a258e538b96)
if(hl_target STREQUAL "esp32p4")
    hl_patched_component(esp_hosted 3.1.0 6879c4babc9c1b8888dec9a019c497980f660395950cbb0a342f95d1cbc591f0)
endif()
project(hdmilight_v3)

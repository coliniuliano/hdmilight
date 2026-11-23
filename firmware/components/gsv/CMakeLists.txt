# Wraps ../../../gsv2001 (the Gscoolink splitter SDK recreated from product firmwares, plus its application layer)
# as an ESP-IDF component. Only the oneboard build (CONFIG_HL_BOARD_ONEBOARD) compiles it; the dev-kit builds have an
# external splitter and get an empty component.
set(GSV_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../gsv2001)
set(GSV_SRCS "")
if(CONFIG_HL_BOARD_ONEBOARD)
    file(GLOB GSV_SRCS ${GSV_DIR}/sdk/*.c ${GSV_DIR}/app/*.c)
endif()
idf_component_register(SRCS ${GSV_SRCS}
                       INCLUDE_DIRS ${GSV_DIR}/sdk ${GSV_DIR}/app)
if(GSV_SRCS)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-parameter -Wno-error)
endif()

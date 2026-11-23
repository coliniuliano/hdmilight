# Wraps ../../../core as an ESP-IDF component (no ESP dependencies inside core).
set(HL_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../core)
file(GLOB HL_CORE_SRCS ${HL_CORE_DIR}/src/*.c)
idf_component_register(SRCS ${HL_CORE_SRCS}
                       INCLUDE_DIRS ${HL_CORE_DIR}/include)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-parameter)

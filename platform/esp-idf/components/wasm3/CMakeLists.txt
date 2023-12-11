file(GLOB_RECURSE M3_SOURCES "wasm3/*.c")

idf_component_register(SRCS ${M3_SOURCES}
                    INCLUDE_DIRS "wasm3")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Os -DESP32 -Dd_m3MaxFunctionStackHeight=256)

# Disable harmless warnings
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-missing-field-initializers -Wno-attributes -Wno-strict-aliasing)

file(GLOB_RECURSE LIB_SOURCES "src/*.c")

# Choose the supervisor/wsh or supervisor/default
idf_component_register(
    SRCS "${LIB_SOURCES}"
    INCLUDE_DIRS "src/include" "include" "supervisor/wsh"
    REQUIRES c9 cwalk json-maker platform tiny-json wasm3
    )

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-parentheses -DDEBUG=1)

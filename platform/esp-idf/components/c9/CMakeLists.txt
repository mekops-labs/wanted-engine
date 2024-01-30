idf_component_register(
    SRCS "c9/c9.c"
    INCLUDE_DIRS c9
    )

target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat=0)

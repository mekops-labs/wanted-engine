file(GLOB_RECURSE ROMFS_SOURCES "romfs/src/*.c")

idf_component_register(
    SRCS "${ROMFS_SOURCES}"
    INCLUDE_DIRS "romfs/include"
    )

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-parentheses)

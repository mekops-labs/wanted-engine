set(sources
    linux/vfs/vfs-linux.c)

add_library(platform STATIC
    ${sources}
)

target_include_directories(platform
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    )

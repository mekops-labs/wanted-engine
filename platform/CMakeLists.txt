set(sources
    linux/vfs/vfs-linux.c
    linux/api/clock.c
    linux/api/random.c
    linux/api/vfs.c
    )

add_library(platform STATIC
    ${sources}
)

target_include_directories(platform
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cwalk/include
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

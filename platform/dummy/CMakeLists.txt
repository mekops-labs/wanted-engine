set(sources
    dummy.c
    dummy-fs.c
)
add_library(platform_dummy STATIC
    ${sources}
)

target_include_directories(platform_dummy
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/platform/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

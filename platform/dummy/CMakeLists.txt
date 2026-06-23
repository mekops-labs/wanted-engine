set(sources
    dummy.c
    dummy-fs.c
    dummy-gpio.c
    dummy-wifi.c
    dummy-extram.c
    dummy-registry.c
    dummy-wapps.c
    dummy-net.c
)
add_library(platform_dummy STATIC
    ${sources}
)

target_compile_options(platform_dummy PRIVATE ${WANTED_STRICT_WARNINGS})

target_include_directories(platform_dummy
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/platform/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

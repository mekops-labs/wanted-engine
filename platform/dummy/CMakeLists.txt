set(sources
    dummy.c
    dummy-crypto.c
    dummy-fs.c
    dummy-extram.c
    dummy-registry.c
    dummy-wapps.c
    dummy-net.c
    dummy-ota.c
    ${CMAKE_SOURCE_DIR}/platform/posix/sha256.c
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

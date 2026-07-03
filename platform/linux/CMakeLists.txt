find_package(OpenSSL)
if(OPENSSL_FOUND)
    option(SECURE_SOCKETS "Build with OpenSSL based secure sockets" TRUE)
endif()

set(sources
    vfs/vfs-linux.c
    api/clock-sleep.c
    api/extram.c
    api/fs-volume.c
    ${CMAKE_SOURCE_DIR}/platform/posix/clock.c
    ${CMAKE_SOURCE_DIR}/platform/posix/fs.c
    ${CMAKE_SOURCE_DIR}/platform/posix/mutex.c
    ${CMAKE_SOURCE_DIR}/platform/posix/registry-store.c
    ${CMAKE_SOURCE_DIR}/platform/posix/socket.c
    ${CMAKE_SOURCE_DIR}/platform/posix/wapps-image.c
    api/crypto.c
    api/random.c
    api/registry.c
    api/wapps.c
)

if (SECURE_SOCKETS)
    set (sources
        ${sources}
        api/ssocket.c
    )
endif ()


add_library(platform_linux STATIC
    ${sources}
)

target_compile_options(platform_linux PRIVATE ${WANTED_STRICT_WARNINGS})

target_link_libraries(platform_linux
    PRIVATE pthread
)

if (SECURE_SOCKETS)
    target_link_libraries(platform_linux
        PRIVATE ${OPENSSL_LIBRARIES}
    )
    target_compile_definitions(platform_linux PRIVATE SECURE_SOCKETS=1)
else()
    target_compile_definitions(platform_linux PRIVATE SECURE_SOCKETS=0)
endif ()

target_include_directories(platform_linux
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/platform/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cwalk/include
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (WANTED_DEBUG_TRACES)
    target_compile_definitions(platform_linux PRIVATE DEBUG=1)
endif()

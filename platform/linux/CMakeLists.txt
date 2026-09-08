# TLS backend for this platform: Linux answers with OpenSSL. REQUIRED on
# purpose — a configuration that asked for TLS must not quietly produce a
# binary without it.
if(CONFIG_WANTED_VFS_SOCKET_TLS)
    # Static only when the CLI itself links -static (cmd/CMakeLists.txt):
    # forcing it otherwise pulls in a static OpenSSL built for a fully static
    # link (may need -lz/-lzstd we never add) when a normal dynamic one would
    # have resolved cleanly.
    if(CONFIG_WANTED_BUILD_STATIC_CLI)
        set(OPENSSL_USE_STATIC_LIBS TRUE)
    endif()
    find_package(OpenSSL REQUIRED)
    set(SECURE_SOCKETS TRUE)
else()
    set(SECURE_SOCKETS FALSE)
endif()

set(sources
    vfs/vfs-linux.c
    api/clock-sleep.c
    api/gpio.c
    api/uart.c
    api/extram.c
    api/fs-volume.c
    ${CMAKE_SOURCE_DIR}/platform/posix/clock.c
    ${CMAKE_SOURCE_DIR}/platform/posix/fs.c
    ${CMAKE_SOURCE_DIR}/platform/posix/mutex.c
    ${CMAKE_SOURCE_DIR}/platform/posix/wake.c
    ${CMAKE_SOURCE_DIR}/platform/posix/persist-mem.c
    ${CMAKE_SOURCE_DIR}/platform/posix/reset-reason.c
    ${CMAKE_SOURCE_DIR}/platform/posix/registry-store.c
    ${CMAKE_SOURCE_DIR}/platform/posix/socket.c
    ${CMAKE_SOURCE_DIR}/platform/posix/wapps-image.c
    ${CMAKE_SOURCE_DIR}/platform/posix/sha256.c
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

if (CONFIG_WANTED_VFS_OTA)
    set (sources
        ${sources}
        api/ota.c
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

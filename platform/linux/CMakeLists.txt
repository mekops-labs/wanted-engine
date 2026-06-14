find_package(OpenSSL)
if(OPENSSL_FOUND)
    option(SECURE_SOCKETS "Build with OpenSSL based secure sockets" TRUE)
endif()

set(sources
    vfs/vfs-linux.c
    api/clock.c
    api/fs.c
    ${CMAKE_SOURCE_DIR}/platform/posix/mutex.c
    api/random.c
    api/registry.c
    api/wapps.c
    api/socket.c
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

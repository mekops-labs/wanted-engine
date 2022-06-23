find_package(OpenSSL)
if(OPENSSL_FOUND)
    option(SECURE_SOCKETS "Build with OpenSSL based secure sockets" TRUE)
endif()

set(sources
    linux/vfs/vfs-linux.c
    linux/api/clock.c
    linux/api/random.c
    linux/api/registry.c
    linux/api/vfs.c
    linux/api/wapps.c
    linux/api/socket.c
)

if (SECURE_SOCKETS)
    set (sources
        ${sources}
        linux/api/ssocket.c
    )
endif ()


add_library(platform STATIC
    ${sources}
)

target_link_libraries(platform
    PRIVATE pthread
)

if (SECURE_SOCKETS)
    target_link_libraries(platform
        PRIVATE ${OPENSSL_LIBRARIES}
    )
    target_compile_definitions(platform PRIVATE SECURE_SOCKETS=1)
else()
    target_compile_definitions(platform PRIVATE SECURE_SOCKETS=0)
endif ()

target_include_directories(platform
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cwalk/include
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(platform
    PRIVATE linux/include
)

if (WANTED_DEBUG_TRACES)
    target_compile_definitions(platform PRIVATE DEBUG=1)
endif()
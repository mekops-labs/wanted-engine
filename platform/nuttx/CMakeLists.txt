set(sources
    vfs/vfs-nuttx.c
    api/clock.c
    api/random.c
    api/mutex.c
    api/wapps.c
    api/memory.c
    api/registry.c
    api/fs.c
    api/socket.c
)

add_library(platform_nuttx STATIC
    ${sources}
)

# Sim links the host pthread; HW gets threads from NuttX libc.
target_link_libraries(platform_nuttx
    PRIVATE pthread
)

target_compile_definitions(platform_nuttx PRIVATE
    PLATFORM_NUTTX=1
    SECURE_SOCKETS=0
)

target_include_directories(platform_nuttx
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/platform/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cwalk/include
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (WANTED_DEBUG_TRACES)
    target_compile_definitions(platform_nuttx PRIVATE DEBUG=1)
endif()

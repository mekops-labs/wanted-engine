set(sources
    vfs/vfs-nuttx.c
    vfs/vfs-gpio.c
    vfs/vfs-wifi.c
    api/clock-sleep.c
    api/extram.c
    api/random.c
    ${CMAKE_SOURCE_DIR}/platform/posix/clock.c
    ${CMAKE_SOURCE_DIR}/platform/posix/fs.c
    ${CMAKE_SOURCE_DIR}/platform/posix/mutex.c
    ${CMAKE_SOURCE_DIR}/platform/posix/registry-store.c
    ${CMAKE_SOURCE_DIR}/platform/posix/socket.c
    ${CMAKE_SOURCE_DIR}/platform/posix/wapps-image.c
    ${CMAKE_SOURCE_DIR}/platform/posix/sha256.c
    api/wapps.c
    api/crypto.c
    api/memory.c
    api/registry.c
    api/fs-volume.c
)

add_library(platform_nuttx STATIC
    ${sources}
)

target_compile_options(platform_nuttx PRIVATE ${WANTED_STRICT_WARNINGS})

# Sim links the host pthread; HW gets threads from NuttX libc.
target_link_libraries(platform_nuttx
    PRIVATE pthread
)

target_compile_definitions(platform_nuttx PRIVATE
    PLATFORM_NUTTX=1
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

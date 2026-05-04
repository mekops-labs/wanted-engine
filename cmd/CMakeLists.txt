add_executable(${project}-cli ${CMAKE_CURRENT_SOURCE_DIR}/main.c)

target_include_directories(${project}-cli PRIVATE ${CMAKE_SOURCE_DIR}/src/include)

option(STATIC_EXECUTABLE "Static build of the executable" OFF)
if (STATIC_EXECUTABLE)
    target_link_libraries(${project}-cli PUBLIC "-static")  #static executable
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/platform/linux ${CMAKE_BINARY_DIR}/platform/linux)

target_link_libraries(${project}-cli
    PRIVATE ${project}
    PRIVATE platform_linux
)

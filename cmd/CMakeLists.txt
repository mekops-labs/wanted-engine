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

add_custom_target(smoke
    COMMAND ${CMAKE_SOURCE_DIR}/test/smoke.sh
            $<TARGET_FILE:${project}-cli>
            ${CMAKE_SOURCE_DIR}/docs/example_config_smoke.json
    DEPENDS ${project}-cli
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running VFS smoke tests via wsh"
)

add_test(
    NAME smoke
    COMMAND ${CMAKE_SOURCE_DIR}/test/smoke.sh
            $<TARGET_FILE:${project}-cli>
            ${CMAKE_SOURCE_DIR}/docs/example_config_smoke.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(smoke PROPERTIES TIMEOUT 30)

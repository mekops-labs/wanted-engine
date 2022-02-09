add_executable(${project}_cmd ${CMAKE_CURRENT_SOURCE_DIR}/main.c)

target_link_libraries(${project}_cmd
    PRIVATE pthread
    PRIVATE ${project}
    )

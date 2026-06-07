add_executable(${project}-cli ${CMAKE_CURRENT_SOURCE_DIR}/main.c)

target_include_directories(${project}-cli PRIVATE ${CMAKE_SOURCE_DIR}/src/include)

option(STATIC_EXECUTABLE "Static build of the executable" OFF)
if (STATIC_EXECUTABLE)
    target_link_libraries(${project}-cli PUBLIC "-static")  #static executable
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/platform/${WANTED_PLATFORM}
                 ${CMAKE_BINARY_DIR}/platform/${WANTED_PLATFORM})

target_link_libraries(${project}-cli
    PRIVATE ${project}
    PRIVATE platform_${WANTED_PLATFORM}
)

# Supervisor wapp smoke test. Always available as a manual target. Only added
# to ctest when WANTED_DEBUG_TRACES is on — without traces the engine is
# silent on failures (FATAL is a no-op), so a passing ctest run with traces
# off would be a false signal of correctness.
add_custom_target(smoke-engine
    COMMAND ${CMAKE_SOURCE_DIR}/test/smoke-engine.sh
            $<TARGET_FILE:${project}-cli>
            ${CMAKE_SOURCE_DIR}/test/smoke-engine-config.json
    DEPENDS ${project}-cli
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running supervisor wapp smoke test"
)

if (WANTED_DEBUG_TRACES)
    add_test(
        NAME smoke-engine
        COMMAND ${CMAKE_SOURCE_DIR}/test/smoke-engine.sh
                $<TARGET_FILE:${project}-cli>
                ${CMAKE_SOURCE_DIR}/test/smoke-engine-config.json
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(smoke-engine PROPERTIES TIMEOUT 30)
endif()

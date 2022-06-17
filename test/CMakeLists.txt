enable_testing()

option(UNITY_EXTENSION_FIXTURE "Compiles Unity with the \"fixture\" extension." ON)
add_subdirectory(unity)

file(GLOB test_src "./*.c")

execute_process(
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND  ${CMAKE_CURRENT_SOURCE_DIR}/generate_runner.sh
             ${CMAKE_CURRENT_BINARY_DIR}/runner.c
)

set(tiny_json_src
    ${CMAKE_SOURCE_DIR}/vendor/tiny-json/tiny-json.c
)

if (CMAKE_COMPILER_IS_GNUCC)
    option(ENABLE_CODE_COVERAGE "Enable build with code coverage analysis." OFF)
    if(ENABLE_CODE_COVERAGE)
        message("-- Code coverage build enabled")
        SET(CMAKE_BUILD_TYPE Debug)
        include(${CMAKE_SOURCE_DIR}/cmake/CodeCoverage.cmake)
        SET(CMAKE_CXX_FLAGS "-g -O0 --coverage -fprofile-arcs -ftest-coverage")
        SET(CMAKE_C_FLAGS "-g -O0 --coverage -fprofile-arcs -ftest-coverage")

        SETUP_TARGET_FOR_COVERAGE_COBERTURA(coverage ctest coverage)
    endif ()
endif()

add_executable(tests ${test_src} ${tiny_json_src} ${CMAKE_CURRENT_BINARY_DIR}/runner.c)
target_link_libraries(tests unity ${project})

execute_process(
    COMMAND grep TEST_GROUP ${CMAKE_CURRENT_BINARY_DIR}/runner.c
    COMMAND bash "-c" "sed -e 's/.*RUN_TEST_GROUP(\\(.*\\));/\\1/'"
    COMMAND xargs
    OUTPUT_VARIABLE TEST_LIST
 )

string (REPLACE " " ";" TEST_LIST "${TEST_LIST}")
string (REPLACE "\n" "" TEST_LIST "${TEST_LIST}")

foreach(TEST_NAME ${TEST_LIST})
    message("-- Testing: adding test group \"${TEST_NAME}\"")
    add_test(${TEST_NAME} tests -g ${TEST_NAME})
endforeach()

target_include_directories(tests PUBLIC ${unity_root}/src)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/src/include)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/vendor/romfs-lib/include)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/vendor/tiny-json)

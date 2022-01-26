option(UNITY_EXTENSION_FIXTURE "Compiles Unity with the \"fixture\" extension." ON)
add_subdirectory(unity)

file(GLOB test_src "./test-*.c")

add_executable(tests ${test_src} test_runners/runner.c)
target_link_libraries(tests unity ${target})

EXECUTE_PROCESS(
    COMMAND grep TEST_GROUP ${CMAKE_SOURCE_DIR}/test/test_runners/runner.c
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

if (wasiType STREQUAL "Romfs" OR wasiType STREQUAL "Vfs")
target_compile_definitions(tests PRIVATE WANTED_ROMFS=1)
endif()

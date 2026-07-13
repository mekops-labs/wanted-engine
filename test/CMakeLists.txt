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

# vendor/ed25519 (verify-only subset): compiled here so
# test-vendor-ed25519.c can validate it directly against RFC 8032 vectors,
# independent of PlatformEd25519Verify/platform wiring. Not linked into
# libwanted.a - Linux's PlatformEd25519Verify stays OpenSSL-backed
# (platform/linux/api/crypto.c); this vendor library is only used by
# platform/nuttx/api/crypto.c, built through NuttX's own Makefile, not CMake.
set(ed25519_vendor_src
    ${CMAKE_SOURCE_DIR}/vendor/ed25519/src/verify.c
    ${CMAKE_SOURCE_DIR}/vendor/ed25519/src/sha512.c
    ${CMAKE_SOURCE_DIR}/vendor/ed25519/src/ge.c
    ${CMAKE_SOURCE_DIR}/vendor/ed25519/src/sc.c
    ${CMAKE_SOURCE_DIR}/vendor/ed25519/src/fe.c
)

# Coverage instrumentation flags are applied at the top-level CMakeLists when
# ENABLE_CODE_COVERAGE is ON. Here we only wire the `coverage` report target.
if(ENABLE_CODE_COVERAGE)
    include(${CMAKE_SOURCE_DIR}/cmake/CodeCoverage.cmake)
    SETUP_TARGET_FOR_COVERAGE_COBERTURA(coverage test/tests coverage)
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/platform/dummy ${CMAKE_BINARY_DIR}/platform/dummy)

add_executable(tests ${test_src} ${tiny_json_src} ${ed25519_vendor_src} ${CMAKE_CURRENT_BINARY_DIR}/runner.c)
target_link_libraries(tests unity ${project} platform_dummy)

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
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/vendor/tiny-json)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/vendor/ed25519/src)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/platform/dummy)
target_include_directories(tests PUBLIC ${CMAKE_SOURCE_DIR}/platform/include)

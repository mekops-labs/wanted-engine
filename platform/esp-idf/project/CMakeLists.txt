cmake_minimum_required(VERSION 3.16)

# WAMR (vendored) as an ESP-IDF component, plus the WANTED engine component.
set(WAMR_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../vendor/wamr)
list(APPEND EXTRA_COMPONENT_DIRS
     ${WAMR_PATH}/build-scripts/esp-idf
     ${CMAKE_CURRENT_LIST_DIR}/../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wanted-esp-idf)

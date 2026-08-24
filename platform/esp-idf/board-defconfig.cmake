# Resolves this ESP-IDF build's engine defconfig and OTA flash-layout.
# Included from both project/main/CMakeLists.txt and
# components/wanted_engine/CMakeLists.txt — reading it twice costs nothing,
# and keeps both from drifting to a different notion of "this board".
#
# WANTED_DEFCONFIG is also cmake/Kconfig.cmake's seed variable: setting it
# here (before that include) is what makes CONFIG_WANTED_MAX_WAPPS and the
# rest of the compiled-in envelope match the generated partition table below.
if(NOT WANTED_DEFCONFIG)
    if(IDF_TARGET STREQUAL "esp32")
        set(WANTED_DEFCONFIG "esp32-esp-idf_defconfig")
    else()
        set(WANTED_DEFCONFIG "xiao_esp32s3_defconfig")
    endif()
endif()
set(WANTED_DEFCONFIG "${WANTED_DEFCONFIG}" CACHE STRING "Engine defconfig" FORCE)

# Classic ESP32 (4 MB flash): single factory app slot, no A/B. Every S3 board
# (8 MB flash): A/B. Chip-derived, not board-derived — no board today needs
# the other chip's layout.
if(IDF_TARGET STREQUAL "esp32")
    set(WANTED_OTA_LAYOUT "factory")
else()
    set(WANTED_OTA_LAYOUT "ab")
endif()

# Optional board-specific extras beyond the defconfig (e.g. extra factory
# seeds), keyed 1:1 by defconfig name so there is one name for "this board".
set(_wanted_board_extras
    "${CMAKE_CURRENT_LIST_DIR}/board-extras/${WANTED_DEFCONFIG}.cmake")
if(EXISTS "${_wanted_board_extras}")
    include("${_wanted_board_extras}")
endif()

# ESP-IDF OTA flash-layout profile: the edge MCU of the Telegraph display.
# Select with -DOTA_PROFILE=s3-telegraph (idf.py build) or
# OTA_PROFILE=s3-telegraph (just build).
#
# Same A/B layout as s3-wapps. What differs is the serial-port driver, the
# listening socket the wapps of the device need, and a registry envelope of 12
# slots at a 256 KiB image ceiling, which is what a supervisor image takes.
set(WANTED_DEFCONFIG "xiao_esp32s3-telegraph_defconfig" CACHE STRING "Engine defconfig" FORCE)
set(WANTED_OTA_LAYOUT "ab")

# The wapps of the device arrive from the control plane, into the registry
# partition. Nothing of them belongs in the app slot, thus this profile seeds
# none of them, with one exception.
#
# The reader of the logs is seeded: it is what a board with no console answers
# questions with, and a board whose registry path is broken is exactly when
# those questions get asked. TELEGRAPH_WAPPS gives the directory that holds it.
if(NOT TELEGRAPH_WAPPS)
    set(TELEGRAPH_WAPPS "$ENV{TELEGRAPH_WAPPS}")
endif()

if(TELEGRAPH_WAPPS)
    set(WANTED_EXTRA_SEEDS "tg-logs:3=${TELEGRAPH_WAPPS}/tg-logs/tg-logs.wasm"
        CACHE STRING "Seed images from outside this repository" FORCE)
endif()

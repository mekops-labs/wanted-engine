# ESP-IDF OTA flash-layout profile: the edge MCU of the Telegraph display.
# Select with -DOTA_PROFILE=s3-telegraph (idf.py build) or
# OTA_PROFILE=s3-telegraph (just build).
#
# Same A/B layout and wapp-slot envelope as s3-wapps. What differs is the
# serial-port driver, and the wapps of the device, which are built in the
# Telegraph firmware repository and seeded from there.
#
# TELEGRAPH_WAPPS gives the path of that repository's wapps directory. A build
# without it carries the engine's own fixtures alone.
set(WANTED_DEFCONFIG "xiao_esp32s3-telegraph_defconfig" CACHE STRING "Engine defconfig" FORCE)
set(WANTED_OTA_LAYOUT "ab")

if(NOT TELEGRAPH_WAPPS)
    set(TELEGRAPH_WAPPS "$ENV{TELEGRAPH_WAPPS}")
endif()

if(TELEGRAPH_WAPPS)
    set(WANTED_EXTRA_SEEDS
        "tg-broker=${TELEGRAPH_WAPPS}/tg-broker/tg-broker.wasm"
        "tg-probe=${TELEGRAPH_WAPPS}/tg-probe/tg-probe.wasm"
        "tg-ota=${TELEGRAPH_WAPPS}/tg-ota/tg-ota.wasm"
        CACHE STRING "Seed images from outside this repository" FORCE)
else()
    message(WARNING
        "s3-telegraph: TELEGRAPH_WAPPS is unset, so the image carries no "
        "wapp of the device. Point it at <telegraph-fw>/wapps.")
endif()

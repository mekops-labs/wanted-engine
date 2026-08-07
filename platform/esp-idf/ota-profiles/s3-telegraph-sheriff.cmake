# ESP-IDF OTA flash-layout profile: the edge MCU of the Telegraph display,
# supervised by the control plane. Select with -DOTA_PROFILE=s3-telegraph-sheriff
# (idf.py build) or OTA_PROFILE=s3-telegraph-sheriff (just build).
#
# Same layout and drivers as s3-telegraph. What differs is the supervisor, its
# launch config, and joining the network at boot, since the supervisor reaches
# its control plane over TCP and no shell is present to bring the radio up.
set(WANTED_DEFCONFIG "xiao_esp32s3-telegraph-sheriff_defconfig" CACHE STRING "Engine defconfig" FORCE)
set(WANTED_OTA_LAYOUT "ab")

# The reader of the logs, seeded for the same reason as in s3-telegraph: a board
# with no console needs one wapp it can always start.
if(NOT TELEGRAPH_WAPPS)
    set(TELEGRAPH_WAPPS "$ENV{TELEGRAPH_WAPPS}")
endif()

if(TELEGRAPH_WAPPS)
    set(WANTED_EXTRA_SEEDS "tg-logs:3=${TELEGRAPH_WAPPS}/tg-logs/tg-logs.wasm"
        CACHE STRING "Seed images from outside this repository" FORCE)
endif()

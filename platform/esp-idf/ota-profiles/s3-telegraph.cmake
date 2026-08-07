# ESP-IDF OTA flash-layout profile: the edge MCU of the Telegraph display.
# Select with -DOTA_PROFILE=s3-telegraph (idf.py build) or
# OTA_PROFILE=s3-telegraph (just build).
#
# Same A/B layout and wapp-slot envelope as s3-wapps. What differs is the
# serial-port driver and the listening socket the wapps of the device need.
set(WANTED_DEFCONFIG "xiao_esp32s3-telegraph_defconfig" CACHE STRING "Engine defconfig" FORCE)
set(WANTED_OTA_LAYOUT "ab")

# The wapps of the device arrive from the control plane, into the registry
# partition. Nothing of them belongs in the app slot, thus this profile seeds
# none of them: WANTED_EXTRA_SEEDS stays a bootstrap mechanism for a board that
# must run something before it has ever reached a network.

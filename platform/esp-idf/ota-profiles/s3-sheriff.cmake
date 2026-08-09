# ESP-IDF OTA flash-layout profile: Sheriff as the supervisor, driven by a
# control plane over Wi-Fi. Select with -DOTA_PROFILE=s3-sheriff (idf.py build)
# or OTA_PROFILE=s3-sheriff (just build).
#
# Same A/B layout and wapp-slot envelope as s3-wapps; the defconfig differs in
# the supervisor it embeds, its launch config, and joining the network at boot.
set(WANTED_DEFCONFIG "xiao_esp32s3-sheriff_defconfig" CACHE STRING "Engine defconfig" FORCE)
set(WANTED_OTA_LAYOUT "ab")

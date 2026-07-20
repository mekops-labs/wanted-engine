# ESP-IDF OTA flash-layout profile: fewer concurrent wapps, a roomier
# "persist" partition (logs, small per-wapp DBs). Select with
# -DOTA_PROFILE=s3-storage (idf.py build).
#
# See s3-wapps.cmake for how the named defconfig drives the rest of the layout.
set(WANTED_DEFCONFIG "psram-s3-storage_defconfig" CACHE STRING "Engine defconfig" FORCE)

# ESP-IDF OTA flash-layout profile: fewer concurrent wapps, a roomier
# "persist" partition (logs, small per-wapp DBs). Select with
# -DOTA_PROFILE=s3-storage (idf.py build).
#
# See s3-wapps.cmake for how MAX_WAPPS drives the rest of the layout.
set(MAX_WAPPS 10 CACHE STRING "Max concurrent wapp instances" FORCE)

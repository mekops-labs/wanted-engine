# ESP-IDF OTA flash-layout profile: max wapp concurrency. Default profile --
# select explicitly with -DOTA_PROFILE=s3-wapps (idf.py build), though this
# is also what an unset OTA_PROFILE resolves to.
#
# MAX_WAPPS drives WAPP_IMAGE_MAX_SLOTS (config-esp-idf.h aliases it
# directly) and, via components/wanted_engine/CMakeLists.txt, the generated
# "wapps"/"persist" partition sizes. FORCE is required: cmake/profiles/psram-s3.cmake already
# set this cache variable earlier in the same configure.
set(MAX_WAPPS 20 CACHE STRING "Max concurrent wapp instances" FORCE)

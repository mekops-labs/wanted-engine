# ESP-IDF OTA flash-layout profile: max wapp concurrency. Default profile --
# select explicitly with -DOTA_PROFILE=s3-wapps (idf.py build), though this
# is also what an unset OTA_PROFILE resolves to.
#
# The engine defconfig named here fixes CONFIG_WANTED_MAX_WAPPS, which drives
# WAPP_IMAGE_MAX_SLOTS (config-esp-idf.h aliases it directly) and, via
# components/wanted_engine/CMakeLists.txt, the generated "wapps"/"persist"
# partition sizes. Naming a defconfig rather than setting the value keeps the
# flash layout and the compiled-in limit derived from one source.
set(WANTED_DEFCONFIG "psram-s3_defconfig" CACHE STRING "Engine defconfig" FORCE)

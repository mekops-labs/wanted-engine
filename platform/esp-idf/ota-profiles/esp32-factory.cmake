# ESP-IDF flash-layout profile for the classic ESP32 (4 MB flash): a single
# factory app slot, no A/B OTA — out of scope for the reference-node build
# (esp_ota_ops calls compile in but have nothing to roll back to). Select
# explicitly with -DOTA_PROFILE=esp32-factory, also the default for chip
# esp32 (see Justfile).
#
# See s3-wapps.cmake for how the named defconfig drives the rest of the
# layout: this fixes CONFIG_WANTED_MAX_WAPPS, which sizes the generated
# "wapps"/"persist" partitions in components/wanted_engine/CMakeLists.txt.
set(WANTED_DEFCONFIG "esp32-esp-idf_defconfig" CACHE STRING "Engine defconfig" FORCE)
set(WANTED_OTA_LAYOUT "factory")

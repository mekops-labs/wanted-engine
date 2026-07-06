# EMBED_FILES links looper.wapp, wifi-connect.wapp and devcheck.wapp straight
# into the app binary as linker sections (`_binary_<name>_wapp_{start,end}`,
# ESP-IDF's standard symbol names for each file's basename). All three are
# smoke-test fixtures: app_main.c factory-seeds them into the flash registry
# at boot so the supervisor can start them without any host-side
# control-plane connection. devcheck exercises /dev/sha256 (and /dev/inflate,
# /dev/ed25519) with known-answer vectors from inside WASM — the M9 proof
# that the hardware-backed PlatformSha256* seam produces a bit-correct digest
# on real silicon, not just "doesn't crash". Prerequisite: `just wapps`, then
# package each wapps/<name>/<name>.wasm per docs/quickstart.md into
# wasm/registry-seed/<name>.wapp (gitignored, like every other packaged .wapp
# — see .gitignore's REGISTRY_ROOT comment).
#
# supervisor.tar (the compiled-in default supervisor) is embedded by the
# wanted_engine component instead — see that CMakeLists.txt — because
# platform.c, which references its symbols, must be linked into the same
# static library as the embed: a symbol EMBED_FILES defines in one component's
# archive is not visible to a different component's archive without
# --start-group/--end-group (this is exactly the link failure that surfaced
# the constraint).
idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/include"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../src/include"
                       REQUIRES wanted_engine
                       PRIV_REQUIRES littlefs esp_netif esp_driver_usb_serial_jtag
                       EMBED_FILES
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/looper.wapp"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/wifi-connect.wapp"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/devcheck.wapp")

# EMBED_FILES links looper.wapp straight into the app binary as a linker
# section (`_binary_looper_wapp_{start,end}`, ESP-IDF's standard symbol names
# for a file named "looper.wapp"). It is an M7 smoke-test fixture: app_main.c
# factory-seeds it into the flash registry at boot so the supervisor can
# start/stop it without any host-side control-plane connection.
# Prerequisite: `just wapps`, then package wapps/looper/looper.wasm per
# docs/quickstart.md into wasm/registry-seed/looper.wapp (gitignored, like
# every other packaged .wapp — see .gitignore's REGISTRY_ROOT comment).
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
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/looper.wapp")

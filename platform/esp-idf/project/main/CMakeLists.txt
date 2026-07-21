# EMBED_FILES links looper.wapp, wifi-connect.wapp and devcheck.wapp straight
# into the app binary as linker sections (`_binary_<name>_wapp_{start,end}`,
# ESP-IDF's standard symbol names for each file's basename). All three are
# smoke-test fixtures: app_main.c factory-seeds them into the flash registry
# at boot so the supervisor can start them without any host-side
# control-plane connection. devcheck exercises /dev/sha256 (and /dev/inflate,
# /dev/ed25519) with known-answer vectors from inside WASM
# that the hardware-backed PlatformSha256* seam produces a bit-correct digest
# on real silicon, not just "doesn't crash". The images are packaged below from
# wapps/<name>/<name>.wasm and are gitignored build outputs, like every other
# packaged .wapp — see .gitignore's REGISTRY_ROOT comment.
#
# supervisor.tar (the compiled-in default supervisor) is embedded by the
# wanted_engine component instead — see that CMakeLists.txt — because
# platform.c, which references its symbols, must be linked into the same
# static library as the embed: a symbol EMBED_FILES defines in one component's
# archive is not visible to a different component's archive without
# --start-group/--end-group (this is exactly the link failure that surfaced
# the constraint).
# Packaged at configure time so EMBED_FILES below is given files that exist, and
# so `idf.py build` works directly. The script leaves up-to-date images alone, so
# this does not relink every build.
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_seed_dir "${_repo}/wasm/registry-seed")
set(_seeds looper wifi-connect devcheck)
foreach(_seed IN LISTS _seeds)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_repo}/wapps/${_seed}/${_seed}.wasm")
endforeach()
execute_process(
    COMMAND "${_repo}/platform/esp-idf/registry-seed.sh" "${_repo}" "${_seed_dir}" ${_seeds}
    RESULT_VARIABLE _seed_rc
    OUTPUT_VARIABLE _seed_out
    ERROR_VARIABLE _seed_err)
if(NOT _seed_rc EQUAL 0)
    message(FATAL_ERROR "packaging the factory-seed images failed:\n${_seed_out}${_seed_err}")
endif()

# Launch config as a blob — no filesystem holds one at first boot.
# EMBED_TXTFILES, not EMBED_FILES: it appends the NUL app_main.c needs. Staged
# under a fixed name so the embed symbol survives a change of file.
#
# A cache variable, not a CONFIG_ symbol: embedded in a host tree the engine's
# .config is narrowed to Kconfig.engine, so the target half is invisible here.
if(NOT WANTED_DEFAULT_CONFIG)
    set(WANTED_DEFAULT_CONFIG "configs/example_config.json")
    set(_cfg_origin "default")
else()
    set(_cfg_origin "configured")
endif()
set(_cfg_staged "${CMAKE_CURRENT_BINARY_DIR}/wanted-config.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_repo}/${WANTED_DEFAULT_CONFIG}")
execute_process(
    COMMAND "${_repo}/utils/default-config.sh" "${_repo}"
            "${WANTED_DEFAULT_CONFIG}" "${_cfg_staged}"
    RESULT_VARIABLE _cfg_rc
    OUTPUT_VARIABLE _cfg_out
    ERROR_VARIABLE _cfg_err)
if(NOT _cfg_rc EQUAL 0)
    message(FATAL_ERROR "staging the default configuration failed:\n${_cfg_out}${_cfg_err}")
endif()
message(STATUS "Default configuration: ${WANTED_DEFAULT_CONFIG} (${_cfg_origin})")

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
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/devcheck.wapp"
                       EMBED_TXTFILES "${_cfg_staged}")

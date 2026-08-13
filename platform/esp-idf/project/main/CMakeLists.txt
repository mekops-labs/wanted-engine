# EMBED_FILES links each seed image straight into the app binary as a linker
# section (`_binary_<name>_wapp_{start,end}`, ESP-IDF's standard symbol names
# for each file's basename). app_main.c factory-seeds them into the flash
# registry at boot so the supervisor can start them without any host-side
# control-plane connection. A seed is written only when its ref is absent, so
# an image installed over a seeded ref survives the next boot. looper,
# wifi-connect, devcheck and blink are smoke-test fixtures; flasher installs
# engine firmware and must be present before any network is up.
# devcheck exercises /dev/sha256 (and /dev/inflate,
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
# What a board cannot recover without: the firmware installer, and the way back
# onto a network. looper, blink and devcheck are bring-up tools and each held a
# 256 KiB registry slot the control plane could use.
set(_seeds wifi-connect flasher)
foreach(_seed IN LISTS _seeds)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_repo}/wapps/${_seed}/${_seed}.wasm")
endforeach()
# The flash-layout profile also names the board's extra seeds, and component
# order does not say whether the engine component has read it yet. Reading it
# here as well costs nothing: a profile file sets cache variables only.
if(NOT OTA_PROFILE)
    set(OTA_PROFILE "s3-wapps")
endif()
include("${_repo}/platform/esp-idf/ota-profiles/${OTA_PROFILE}.cmake")

# A board carries wapps this repository does not hold: WANTED_EXTRA_SEEDS is a
# list of <ref>=<path to .wasm>, packaged and embedded beside the fixtures
# above. The declarations and the seed calls are generated, so app_main.c names
# no board's wapp.
set(_extra_args "")
set(_extra_decl "")
set(_extra_call "")
set(_extra_embed "")
foreach(_extra IN LISTS WANTED_EXTRA_SEEDS)
    if(NOT _extra MATCHES "^([^=]+)=(.+)$")
        message(FATAL_ERROR "WANTED_EXTRA_SEEDS entry '${_extra}' is not <ref>=<path>")
    endif()
    set(_ref "${CMAKE_MATCH_1}")
    set(_src "${CMAKE_MATCH_2}")
    string(REGEX REPLACE ":.*$" "" _name "${_ref}")
    string(MAKE_C_IDENTIFIER "${_name}" _sym)
    list(APPEND _extra_args "${_name}=${_src}")
    list(APPEND _extra_embed "${_seed_dir}/${_name}.wapp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    string(APPEND _extra_decl
        "extern const uint8_t _binary_${_sym}_wapp_start[];\n"
        "extern const uint8_t _binary_${_sym}_wapp_end[];\n")
    string(APPEND _extra_call
        "seedWapp(\"${_ref}\", _binary_${_sym}_wapp_start,"
        " _binary_${_sym}_wapp_end);\n")
endforeach()

execute_process(
    COMMAND "${_repo}/platform/esp-idf/registry-seed.sh" "${_repo}" "${_seed_dir}"
            ${_seeds} ${_extra_args}
    RESULT_VARIABLE _seed_rc
    OUTPUT_VARIABLE _seed_out
    ERROR_VARIABLE _seed_err)
if(NOT _seed_rc EQUAL 0)
    message(FATAL_ERROR "packaging the factory-seed images failed:\n${_seed_out}${_seed_err}")
endif()

# Written every configure, empty when the board names no extra seed, so
# app_main.c includes them unconditionally.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extra-seeds-decl.inc" "${_extra_decl}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extra-seeds-call.inc" "${_extra_call}")

# The flasher is seeded under the version of the tree it was built from,
# written beside the wasm by the rule that produced it. A registry version
# field holds WAPP_MAX_VERSION_LEN - 1 characters; a longer one truncates into
# a ref naming a version the image is not, so it fails the build.
file(READ "${_repo}/wapps/flasher/flasher.version" _flasher_version)
string(STRIP "${_flasher_version}" _flasher_version)
string(LENGTH "${_flasher_version}" _flasher_version_len)
if(_flasher_version_len GREATER 14)
    message(FATAL_ERROR
        "flasher version '${_flasher_version}' is ${_flasher_version_len} characters; "
        "a registry version field holds 14. Tag the supervisor tree, or raise "
        "WAPP_MAX_VERSION_LEN (it bounds WAPP_MAX_IMAGE_REF_LEN, nothing on the wire).")
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

# The console blocking-driver setup (app_main.c's consoleUseBlockingDriver)
# uses a different peripheral driver per board: USB-Serial/JTAG on the S3,
# UART on the classic part. Require only the one this chip's console uses.
# IDF_TARGET is always defined by the time a component CMakeLists.txt runs,
# no include needed.
set(_priv_requires littlefs esp_netif)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND _priv_requires esp_driver_usb_serial_jtag)
else()
    list(APPEND _priv_requires esp_driver_uart)
endif()

idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/include"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/esp-idf/include"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../src/include"
                       REQUIRES wanted_engine
                       PRIV_REQUIRES ${_priv_requires}
                       EMBED_FILES
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/looper.wapp"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/wifi-connect.wapp"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/devcheck.wapp"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/blink.wapp"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../../wasm/registry-seed/flasher.wapp"
                           ${_extra_embed}
                       EMBED_TXTFILES "${_cfg_staged}")

# The generated seed declarations live in the build directory.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# After registration: a directory-level define runs before the component
# library exists.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WANTED_FLASHER_REF=\"flasher:${_flasher_version}\")

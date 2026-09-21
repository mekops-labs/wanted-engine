# EMBED_FILES links each seed image into the app binary; app_main.c
# factory-seeds them into the flash registry at boot. Written only when its ref
# is absent, so an installed image survives the next boot.

# supervisor.tar is embedded by the wanted_engine component instead — see that
# CMakeLists.txt — since platform.c and the embed must share one static
# library's archive.

# Packaged at configure time so EMBED_FILES below sees files that exist; the
# script leaves up-to-date images alone, so this skips relinking.
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_seed_dir "${_repo}/wasm/registry-seed")

# This board's defconfig also names its extra seeds, and component order does
# not say whether the engine component has resolved it yet. Reading it here as
# well costs nothing: board-defconfig.cmake only sets cache variables.
include("${_repo}/platform/esp-idf/board-defconfig.cmake")

# This board's own factory seeds — a space-separated list of wapps/<name>, each
# built by this repo. Empty by default: only a board whose defconfig names one
# seeds anything.
#
# A private kconfiglib run: cmake/Kconfig.cmake sets CONFIG_ variables as plain,
# so they never cross this component boundary, and reusing its
# CMAKE_CONFIGURE_DEPENDS registration here conflicts under Ninja.
set(_seed_dotconfig "${CMAKE_CURRENT_BINARY_DIR}/seed-wapps.config")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "PYTHONPATH=${_repo}/tools/kconfiglib"
        "KCONFIG_CONFIG=${_seed_dotconfig}" python3
        tools/kconfiglib/defconfig.py --kconfig Kconfig.engine
        "configs/${WANTED_DEFCONFIG}"
    WORKING_DIRECTORY "${_repo}"
    RESULT_VARIABLE _seed_kconfig_rc
    OUTPUT_VARIABLE _seed_kconfig_out
    ERROR_VARIABLE _seed_kconfig_err)
if(NOT _seed_kconfig_rc EQUAL 0)
    message(
        FATAL_ERROR
            "kconfig: resolving WANTED_SEED_WAPPS failed:\n${_seed_kconfig_out}${_seed_kconfig_err}"
    )
endif()
file(STRINGS "${_seed_dotconfig}" _seed_line REGEX "^CONFIG_WANTED_SEED_WAPPS=")
string(REGEX REPLACE "^CONFIG_WANTED_SEED_WAPPS=\"(.*)\"$" "\\1" _seeds
                     "${_seed_line}")
separate_arguments(_seeds)

# A board carries wapps this repo does not hold: WANTED_EXTRA_SEEDS is a list of
# <ref>=<path to .wasm>. Declarations and seed calls are generated, so
# app_main.c names no board's wapp.
set(_extra_args "")
set(_extra_decl "")
set(_extra_call "")
set(_extra_embed "")
foreach(_extra IN LISTS WANTED_EXTRA_SEEDS)
    if(NOT _extra MATCHES "^([^=]+)=(.+)$")
        message(
            FATAL_ERROR
                "WANTED_EXTRA_SEEDS entry '${_extra}' is not <ref>=<path>")
    endif()
    set(_ref "${CMAKE_MATCH_1}")
    set(_src "${CMAKE_MATCH_2}")
    string(REGEX REPLACE ":.*$" "" _name "${_ref}")
    string(MAKE_C_IDENTIFIER "${_name}" _sym)
    list(APPEND _extra_args "${_name}=${_src}")
    list(APPEND _extra_embed "${_seed_dir}/${_name}.wapp")
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    string(APPEND _extra_decl
           "extern const uint8_t _binary_${_sym}_wapp_start[];\n"
           "extern const uint8_t _binary_${_sym}_wapp_end[];\n")
    string(APPEND _extra_call
           "seedWapp(\"${_ref}\", _binary_${_sym}_wapp_start,"
           " _binary_${_sym}_wapp_end);\n")
endforeach()

execute_process(
    COMMAND "${_repo}/platform/esp-idf/registry-seed.sh" "${_repo}"
            "${_seed_dir}" ${_seeds} ${_extra_args}
    RESULT_VARIABLE _seed_rc
    OUTPUT_VARIABLE _seed_out
    ERROR_VARIABLE _seed_err)
if(NOT _seed_rc EQUAL 0)
    message(
        FATAL_ERROR
            "packaging the factory-seed images failed:\n${_seed_out}${_seed_err}"
    )
endif()

# Each seed becomes one EMBED_FILES entry, after packaging so a versioned one
# has registry-seed.sh's <name>@<version>.wapp to embed. A registry version
# field holds WAPP_MAX_VERSION_LEN - 1 characters.
foreach(_seed IN LISTS _seeds)
    string(MAKE_C_IDENTIFIER "${_seed}" _sym)
    set(_ref "${_seed}")
    set(_embed "${_seed_dir}/${_seed}.wapp")
    set(_version_file "${_repo}/wapps/${_seed}/${_seed}.version")
    if(EXISTS "${_version_file}")
        file(READ "${_version_file}" _version)
        string(STRIP "${_version}" _version)
        string(LENGTH "${_version}" _version_len)
        if(_version_len GREATER 14)
            message(
                FATAL_ERROR
                    "${_seed} version '${_version}' is ${_version_len} characters; "
                    "a registry version field holds 14. Tag the tree it is built "
                    "from, or raise WAPP_MAX_VERSION_LEN (it bounds "
                    "WAPP_MAX_IMAGE_REF_LEN, nothing on the wire).")
        endif()
        set(_ref "${_seed}:${_version}")

        # registry-seed.sh names the packaged file by version, for a pinned
        # resolve; the embedded symbol must stay stable across versions instead,
        # so this copies to a version-independent name.
        set(_packaged "${_seed_dir}/${_seed}@${_version}.wapp")
        set(_embed "${CMAKE_CURRENT_BINARY_DIR}/${_seed}.wapp")
        file(COPY_FILE "${_packaged}" "${_embed}" ONLY_IF_DIFFERENT)
        set_property(
            DIRECTORY
            APPEND
            PROPERTY CMAKE_CONFIGURE_DEPENDS "${_packaged}")
    endif()
    list(APPEND _extra_embed "${_embed}")
    string(APPEND _extra_decl
           "extern const uint8_t _binary_${_sym}_wapp_start[];\n"
           "extern const uint8_t _binary_${_sym}_wapp_end[];\n")
    string(APPEND _extra_call
           "seedWapp(\"${_ref}\", _binary_${_sym}_wapp_start,"
           " _binary_${_sym}_wapp_end);\n")
endforeach()

# Written every configure, empty when the board names no seed, so app_main.c
# includes them unconditionally.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extra-seeds-decl.inc" "${_extra_decl}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extra-seeds-call.inc" "${_extra_call}")

# Launch config as a blob — no filesystem holds one at first boot.
# EMBED_TXTFILES appends the NUL app_main.c needs. A cache variable, not a
# CONFIG_ symbol: this host tree's .config is narrowed to Kconfig.engine.
if(NOT WANTED_DEFAULT_CONFIG)
    set(WANTED_DEFAULT_CONFIG "configs/example_config.json")
    set(_cfg_origin "default")
else()
    set(_cfg_origin "configured")
endif()
set(_cfg_staged "${CMAKE_CURRENT_BINARY_DIR}/wanted-config.json")
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${_repo}/${WANTED_DEFAULT_CONFIG}")
execute_process(
    COMMAND "${_repo}/utils/default-config.sh" "${_repo}"
            "${WANTED_DEFAULT_CONFIG}" "${_cfg_staged}"
    RESULT_VARIABLE _cfg_rc
    OUTPUT_VARIABLE _cfg_out
    ERROR_VARIABLE _cfg_err)
if(NOT _cfg_rc EQUAL 0)
    message(
        FATAL_ERROR
            "staging the default configuration failed:\n${_cfg_out}${_cfg_err}")
endif()
message(
    STATUS "Default configuration: ${WANTED_DEFAULT_CONFIG} (${_cfg_origin})")

# The console uses a different peripheral per board: USB-Serial/JTAG on the S3,
# UART on the classic part. IDF_TARGET is always defined by the time a component
# CMakeLists.txt runs, no include needed.
set(_priv_requires littlefs esp_netif)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND _priv_requires esp_driver_usb_serial_jtag)
else()
    list(APPEND _priv_requires esp_driver_uart)
endif()

idf_component_register(
    SRCS
    "app_main.c"
    INCLUDE_DIRS
    "."
    PRIV_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/esp-idf/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../src/include"
    REQUIRES
    wanted_engine
    PRIV_REQUIRES
    ${_priv_requires}
    EMBED_FILES
    ${_extra_embed}
    EMBED_TXTFILES
    "${_cfg_staged}")

# The generated seed declarations live in the build directory.
target_include_directories(${COMPONENT_LIB}
                           PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

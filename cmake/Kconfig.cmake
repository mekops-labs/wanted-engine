# Kconfig-driven build configuration: the root Kconfig plus a .config become
# wanted-autoconf.h (for C) and CMake variables (for conditional source lists).
#
# The .config lives in the build dir, not the source tree — build dirs differ
# in configuration and stay independent. Editing it re-runs configure;
# genconfig skips unchanged writes, so that does not cascade into a rebuild.

find_program(WANTED_PYTHON NAMES python3 python REQUIRED)

# Engine checkout root. Set explicitly by a host that compiles engine sources
# into its own tree (the ESP-IDF component), where the engine is not top-level.
if(WANTED_ENGINE_ROOT)
    set(_wanted_embedded TRUE)
else()
    set(WANTED_ENGINE_ROOT ${CMAKE_SOURCE_DIR})
    set(_wanted_embedded FALSE)
endif()

set(WANTED_KCONFIG_DIR ${WANTED_ENGINE_ROOT}/tools/kconfiglib)

# The header always comes from the engine half alone, so a board string or SDK
# URL can never reach a translation unit; build-system symbols are read off
# .config below, where CMake already parses it.
set(WANTED_KCONFIG_ENGINE ${WANTED_ENGINE_ROOT}/Kconfig.engine)

# .config's root differs by who is driving. Standalone, it spans the whole tree
# so menuconfig and `just build` see the target. Embedded in a host tree, the
# host has already decided the target by construction — offering it the menu
# again would be a second answer to a settled question.
if(_wanted_embedded)
    set(WANTED_KCONFIG_ROOT ${WANTED_KCONFIG_ENGINE})
else()
    set(WANTED_KCONFIG_ROOT ${WANTED_ENGINE_ROOT}/Kconfig)
endif()
set(WANTED_DOTCONFIG ${CMAKE_BINARY_DIR}/.config)
set(WANTED_AUTOCONF_DIR ${CMAKE_BINARY_DIR}/include)
set(WANTED_AUTOCONF ${WANTED_AUTOCONF_DIR}/wanted-autoconf.h)

# The full filename under configs/, suffix included — this is an internal
# variable, set by the OpenWrt packaging script and the ESP-IDF OTA profiles.
# The user-facing DEFCONFIG env var omits the suffix; the Justfile appends it.
set(WANTED_DEFCONFIG "" CACHE STRING
    "Board defconfig under configs/ to seed .config (empty = Kconfig defaults)")

file(MAKE_DIRECTORY ${WANTED_AUTOCONF_DIR})

# Run a kconfiglib entry point against this build dir's .config.
function(_wanted_kconfig_run script)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=${WANTED_KCONFIG_DIR}"
                "KCONFIG_CONFIG=${WANTED_DOTCONFIG}"
                ${WANTED_PYTHON} ${WANTED_KCONFIG_DIR}/${script} ${ARGN}
        WORKING_DIRECTORY ${WANTED_ENGINE_ROOT}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "kconfig: ${script} failed (${_rc})\n${_out}${_err}")
    endif()
endfunction()

# Seed a missing .config from a named defconfig, else from the Kconfig
# defaults. An existing .config is never overwritten — a reconfigure must not
# silently discard a configuration.
if(NOT EXISTS ${WANTED_DOTCONFIG})
    if(WANTED_DEFCONFIG)
        set(_defconfig ${WANTED_ENGINE_ROOT}/configs/${WANTED_DEFCONFIG})
        if(NOT EXISTS ${_defconfig})
            message(FATAL_ERROR
                "kconfig: defconfig not found: ${_defconfig}")
        endif()
        message(STATUS "Kconfig: seeding .config from ${WANTED_DEFCONFIG}")
        _wanted_kconfig_run(defconfig.py --kconfig ${WANTED_KCONFIG_ROOT}
                            ${_defconfig})
    else()
        message(STATUS "Kconfig: seeding .config from Kconfig defaults")
        _wanted_kconfig_run(olddefconfig.py ${WANTED_KCONFIG_ROOT})
    endif()
else()
    # Carry an existing .config over Kconfig edits: new symbols take their
    # default rather than silently reading as n.
    _wanted_kconfig_run(olddefconfig.py ${WANTED_KCONFIG_ROOT})
endif()

_wanted_kconfig_run(genconfig.py --header-path ${WANTED_AUTOCONF}
                    ${WANTED_KCONFIG_ENGINE})

# Reconfigure when any of them changes, so the header cannot go stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${WANTED_DOTCONFIG} ${WANTED_KCONFIG_ROOT} ${WANTED_KCONFIG_ENGINE}
    ${WANTED_ENGINE_ROOT}/Kconfig.target)

# Mirror into CMake variables. `=y` becomes true; ints and strings carry their
# value; "is not set" lines are skipped, leaving the variable undefined.
file(STRINGS ${WANTED_DOTCONFIG} _cfg_lines ENCODING UTF-8)
foreach(_line IN LISTS _cfg_lines)
    if(_line MATCHES "^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
        set(_sym ${CMAKE_MATCH_1})
        set(_val ${CMAKE_MATCH_2})
        if(_val STREQUAL "y")
            set(${_sym} TRUE)
        else()
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" _val "${_val}")
            set(${_sym} "${_val}")
        endif()
    endif()
endforeach()

message(STATUS "Kconfig: ${WANTED_DOTCONFIG} -> ${WANTED_AUTOCONF}")

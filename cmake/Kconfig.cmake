# Kconfig-driven build configuration.
#
# Turns the root Kconfig tree plus a .config into two things the build needs:
#
#   * wanted-autoconf.h — the generated header every C source sees;
#   * CMake variables    — the same symbols, so source lists can be conditional.
#
# The .config lives in the *build* directory, not the source tree: build dirs
# differ in configuration (the debug-supervisor build, the extra-drivers lane,
# a cross build), and one .config per build dir is what keeps them independent.
#
# Editing .config re-runs the configure step, which regenerates the header.
# genconfig skips the write when the contents are unchanged, so an unrelated
# reconfigure does not cascade into a full rebuild.

find_program(WANTED_PYTHON NAMES python3 python REQUIRED)

# The engine checkout root. Defaults to this project's source dir, and is set
# explicitly by a host build that compiles engine sources into its own tree
# (the ESP-IDF component), where the engine is not the top-level project.
if(NOT WANTED_ENGINE_ROOT)
    set(WANTED_ENGINE_ROOT ${CMAKE_SOURCE_DIR})
endif()

set(WANTED_KCONFIG_DIR ${WANTED_ENGINE_ROOT}/tools/kconfiglib)
set(WANTED_KCONFIG_ROOT ${WANTED_ENGINE_ROOT}/Kconfig)
set(WANTED_DOTCONFIG ${CMAKE_BINARY_DIR}/.config)
set(WANTED_AUTOCONF_DIR ${CMAKE_BINARY_DIR}/include)
set(WANTED_AUTOCONF ${WANTED_AUTOCONF_DIR}/wanted-autoconf.h)

set(WANTED_DEFCONFIG "" CACHE STRING
    "Board defconfig under configs/ to seed .config (empty = Kconfig defaults)")

file(MAKE_DIRECTORY ${WANTED_AUTOCONF_DIR})

# Run a kconfiglib entry point with the vendored library on the path and the
# build dir's .config selected.
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

# Seed a missing .config, from a named defconfig when one was requested and from
# the Kconfig defaults otherwise. An existing .config is never overwritten here:
# a reconfigure must not silently discard a configuration.
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
    # Bring an existing .config forward over Kconfig edits: new symbols take
    # their default rather than silently reading as n.
    _wanted_kconfig_run(olddefconfig.py ${WANTED_KCONFIG_ROOT})
endif()

_wanted_kconfig_run(genconfig.py --header-path ${WANTED_AUTOCONF}
                    ${WANTED_KCONFIG_ROOT})

# Re-run the configure step when the configuration or the tree describing it
# changes, so the generated header can never go stale against .config.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${WANTED_DOTCONFIG} ${WANTED_KCONFIG_ROOT})

# Mirror the configuration into CMake variables so source lists can be
# conditional. `CONFIG_X=y` becomes a true variable, `CONFIG_X=<n>` and
# `CONFIG_X="s"` carry their value; "is not set" lines are skipped, leaving the
# variable undefined (and therefore false).
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

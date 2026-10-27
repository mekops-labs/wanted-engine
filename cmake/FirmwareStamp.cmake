# The firmware a build names for /proc/wanted: WANTED_FIRMWARE_BOARD, and the
# first line of WANTED_FIRMWARE_VARIANT_FILE, relative to `base_dir`.

function(wanted_firmware_stamp target base_dir)
    set(_board "${WANTED_FIRMWARE_BOARD}")
    set(_variant "")
    if(WANTED_FIRMWARE_VARIANT_FILE)
        get_filename_component(_file "${WANTED_FIRMWARE_VARIANT_FILE}" ABSOLUTE
                               BASE_DIR "${base_dir}")
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "firmware variant file missing: ${_file}")
        endif()
        file(STRINGS "${_file}" _variant LIMIT_COUNT 1)
        # A changed variant must reconfigure, or the stamp goes stale.
        set_property(
            DIRECTORY
            APPEND
            PROPERTY CMAKE_CONFIGURE_DEPENDS "${_file}")
    endif()
    # The image tag's alphabet; it also keeps the value a plain C string.
    foreach(_value IN ITEMS "${_board}" "${_variant}")
        if(NOT _value STREQUAL "" AND NOT _value MATCHES "^[A-Za-z0-9._]+$")
            message(FATAL_ERROR "firmware name '${_value}' must match "
                                "[A-Za-z0-9._]+")
        endif()
    endforeach()
    message(STATUS "Firmware: ${_board} ${_variant}")
    target_compile_definitions(
        ${target} PRIVATE WANTED_FIRMWARE_BOARD="${_board}"
                          WANTED_FIRMWARE_VARIANT="${_variant}")
endfunction()

# Resolves a defconfig name (suffix included) to a full path. The out-of-tree
# board directory's configs/ is searched before the engine's own.

# From the environment: ESP-IDF's requirements pass runs without the cache.
set(WANTED_BOARD_DIR "$ENV{WANTED_BOARD_DIR}")

get_filename_component(_wanted_engine_configs
                       "${CMAKE_CURRENT_LIST_DIR}/../configs" ABSOLUTE)

function(wanted_defconfig_path out name)
    set(_path ${_wanted_engine_configs}/${name})
    if(WANTED_BOARD_DIR AND EXISTS ${WANTED_BOARD_DIR}/configs/${name})
        set(_path ${WANTED_BOARD_DIR}/configs/${name})
    endif()
    set(${out}
        ${_path}
        PARENT_SCOPE)
endfunction()

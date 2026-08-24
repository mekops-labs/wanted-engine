# Board extra for the Telegraph display's edge MCU: the reader of the logs,
# seeded from outside this repo so a board with no console has one wapp it
# can always start.
if(NOT TELEGRAPH_WAPPS)
    set(TELEGRAPH_WAPPS "$ENV{TELEGRAPH_WAPPS}")
endif()

if(TELEGRAPH_WAPPS)
    set(WANTED_EXTRA_SEEDS "tg-logs:3=${TELEGRAPH_WAPPS}/tg-logs/tg-logs.wasm"
        CACHE STRING "Seed images from outside this repository" FORCE)
endif()

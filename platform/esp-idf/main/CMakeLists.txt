set(APP_SOURCES "main.c")

idf_component_register(SRCS ${APP_SOURCES})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Os -DESP32)


target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-missing-field-initializers -Wno-attributes)

# Fixture out-of-tree driver tree (-DWANTED_EXTRA_DRIVERS_DIR=<this dir>).
# Contract: a library target `wanted_extra_drivers` defining
# ExtraDriverTable(); the engine adds its headers to this target.
add_library(wanted_extra_drivers STATIC fixture-drivers.c)

# Fixture out-of-tree driver tree, wired in with
# -DWANTED_EXTRA_DRIVERS_DIR=<this dir>. It stands in for a real external tree:
# the only contract is a library target named `wanted_extra_drivers` and a
# definition of ExtraDriverTable(). The engine adds its headers to this target.
add_library(wanted_extra_drivers STATIC fixture-drivers.c)

# Supervisor wapp builder.
#
# Each variant subdirectory provides an `app.wasm`, bundled into a POSIX ustar
# archive consumed at runtime via PlatformWappLoad — a supervisor image is just
# `app.wasm`.
#
#   wsh, selftest  — compiled from C under wapps/ with the wasi-sdk clang that
#                    ships in the main build image; built by the default `all`.
#   sheriff        — the production supervisor, a Zig wapp built from the
#                    wapps/sheriff submodule. Zig is NOT in the main build image,
#                    so sheriff is a SEPARATE target (the engine build does not
#                    build it):
#                        make -C wasm/supervisor sheriff
#                    ZIG is overridable — build with host zig / the wapp-sdk
#                    image (default), or via sheriff's own toolchain container:
#                        make -C wasm/supervisor sheriff ZIG='./scripts/dev zig'
#
#   make -C wasm/supervisor                  # wsh + selftest (default)
#   make -C wasm/supervisor sheriff          # the sheriff supervisor (needs zig)
#   make -C wasm/supervisor VARIANT=wsh build

# Default build is the C supervisors (wasi-sdk clang, in the main build image).
# sheriff (Zig, from the submodule) and wifi-connect (a boot-time helper, not
# a general-purpose supervisor) are opt-in so the default build stays minimal.
VARIANTS     ?= wsh selftest
ALL_VARIANTS := $(VARIANTS) sheriff wifi-connect

DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all clean $(ALL_VARIANTS)

all: $(VARIANTS)

# Per-variant rules dispatched via sub-make so VARIANT scopes file paths.
$(ALL_VARIANTS):
	$(MAKE) -f $(DIR)Makefile VARIANT=$@ build

ifdef VARIANT

VARIANT_DIR := $(DIR)$(VARIANT)
TAR         := $(VARIANT_DIR)/supervisor.tar
INPUTS      := app.wasm

.PHONY: build
build: $(TAR)

# Pack app.wasm into a ustar archive — the engine identifies the supervisor by
# its compiled-in name, not by in-image metadata.
$(TAR): $(addprefix $(VARIANT_DIR)/,$(INPUTS))
	tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
	    -C $(VARIANT_DIR) -cf $@ $(INPUTS)

# wsh's app.wasm is compiled from the wapps/wsh/ source tree.
ifeq ($(VARIANT),wsh)
WSH_SRC := $(DIR)../../wapps/wsh
$(VARIANT_DIR)/app.wasm: $(wildcard $(WSH_SRC)/*.c) $(wildcard $(WSH_SRC)/*.h) $(WSH_SRC)/Makefile
	mkdir -p $(VARIANT_DIR)
	$(MAKE) -C $(WSH_SRC)
	cp $(WSH_SRC)/wsh.wasm $@
endif

# selftest (the test supervisor) is compiled from the wapps/selftest/ source.
ifeq ($(VARIANT),selftest)
SELFTEST_SRC := $(DIR)../../wapps/selftest
$(VARIANT_DIR)/app.wasm: $(wildcard $(SELFTEST_SRC)/*.c) $(wildcard $(SELFTEST_SRC)/*.h) $(SELFTEST_SRC)/Makefile
	mkdir -p $(VARIANT_DIR)
	$(MAKE) -C $(SELFTEST_SRC)
	cp $(SELFTEST_SRC)/selftest.wasm $@
endif

# sheriff's app.wasm is built from the wapps/sheriff submodule with Zig
# (build.zig targets wasm32-wasi / ReleaseSmall). ZIG is overridable (see header).
ifeq ($(VARIANT),sheriff)
ZIG         ?= zig
SHERIFF_SRC := $(DIR)../../wapps/sheriff
$(VARIANT_DIR)/app.wasm: $(wildcard $(SHERIFF_SRC)/src/*.zig) $(SHERIFF_SRC)/build.zig
	mkdir -p $(VARIANT_DIR)
	cd $(SHERIFF_SRC) && $(ZIG) build
	cp $(SHERIFF_SRC)/zig-out/bin/sheriff.wasm $@
endif

# wifi-connect's app.wasm is compiled from the wapps/wifi-connect/ source tree.
ifeq ($(VARIANT),wifi-connect)
WIFI_CONNECT_SRC := $(DIR)../../wapps/wifi-connect
$(VARIANT_DIR)/app.wasm: $(wildcard $(WIFI_CONNECT_SRC)/*.c) $(WIFI_CONNECT_SRC)/Makefile
	mkdir -p $(VARIANT_DIR)
	$(MAKE) -C $(WIFI_CONNECT_SRC)
	cp $(WIFI_CONNECT_SRC)/wifi-connect.wasm $@
endif

endif

clean:
	rm -f $(addsuffix /supervisor.tar,$(addprefix $(DIR),$(ALL_VARIANTS))) \
	      $(DIR)sheriff/app.wasm

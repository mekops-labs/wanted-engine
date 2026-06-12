# Supervisor wapp builder.
#
# Each variant subdirectory (sheriff, wsh) provides an `app.wasm`, bundled into
# a POSIX ustar archive consumed at runtime via PlatformWappLoad — no C header
# blob generated. A supervisor image is just `app.wasm`.
#
# `wsh` (the debug shell supervisor) compiles its `app.wasm` from source under
# `wapps/wsh/`. `sheriff` ships as a prebuilt blob — its source lives in a
# separate repository.
#
#   make -C wasm/supervisor                   # rebuilds both variants
#   make -C wasm/supervisor VARIANT=wsh       # rebuild a single variant

VARIANTS ?= sheriff wsh selftest

DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all clean $(VARIANTS)

all: $(VARIANTS)

# Per-variant rules dispatched via sub-make so VARIANT scopes file paths.
$(VARIANTS):
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
	$(MAKE) -C $(WSH_SRC)
	cp $(WSH_SRC)/wsh.wasm $@
endif

# selftest (the test supervisor) is compiled from the wapps/selftest/ source.
ifeq ($(VARIANT),selftest)
SELFTEST_SRC := $(DIR)../../wapps/selftest
$(VARIANT_DIR)/app.wasm: $(wildcard $(SELFTEST_SRC)/*.c) $(wildcard $(SELFTEST_SRC)/*.h) $(SELFTEST_SRC)/Makefile
	$(MAKE) -C $(SELFTEST_SRC)
	cp $(SELFTEST_SRC)/selftest.wasm $@
endif

endif

clean:
	rm -f $(addsuffix /supervisor.tar,$(addprefix $(DIR),$(VARIANTS)))

# Supervisor wapp builder.
#
# Each variant subdirectory (sheriff, wsh) provides an `app.wasm` and
# `manifest.json`, bundled into a POSIX ustar archive consumed at runtime via
# PlatformWappLoad — no C header blob generated.
#
# `wsh` (the debug shell supervisor) compiles its `app.wasm` from source under
# `wapps/wsh/`. `sheriff` ships as a prebuilt blob — its source lives in a
# separate repository.
#
#   make -C wasm/supervisor                   # rebuilds both variants
#   make -C wasm/supervisor VARIANT=wsh       # rebuild a single variant

VARIANTS ?= sheriff wsh

DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all clean $(VARIANTS)

all: $(VARIANTS)

# Per-variant rules dispatched via sub-make so VARIANT scopes file paths.
$(VARIANTS):
	$(MAKE) -f $(DIR)Makefile VARIANT=$@ build

ifdef VARIANT

VARIANT_DIR := $(DIR)$(VARIANT)
TAR         := $(VARIANT_DIR)/supervisor.tar
INPUTS      := app.wasm manifest.json

.PHONY: build
build: $(TAR)

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

endif

clean:
	rm -f $(addsuffix /supervisor.tar,$(addprefix $(DIR),$(VARIANTS)))

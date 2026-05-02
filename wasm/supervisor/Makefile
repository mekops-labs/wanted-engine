# Supervisor wapp builder.
#
# Each variant subdirectory (sheriff, wsh) ships an `app.wasm` and
# `manifest.json`. This Makefile bundles them into a POSIX ustar archive
# consumed at runtime via PlatformWappLoad — no C header blob generated.
#
# Re-run after changing app.wasm / manifest.json:
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

endif

clean:
	rm -f $(addsuffix /supervisor.tar,$(addprefix $(DIR),$(VARIANTS)))

# Supervisor wapp blob builder.
#
# Each variant subdirectory (default, wsh) ships an `app.wasm` and
# `manifest.json`. This Makefile bundles them into a POSIX ustar archive and
# emits `supervisor.h` (a `xxd -i` C array) consumed by src/wanted.c via
# wapp->layers[0]. The output replaces the legacy romfs blob: TarFS now
# parses the layer at boot and pre-fetches the entrypoints.
#
# Re-run after changing app.wasm / manifest.json:
#   make -C wasm/supervisor                 # rebuilds both variants
#   make -C wasm/supervisor VARIANT=wsh     # rebuild a single variant

VARIANTS ?= default wsh

DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all clean $(VARIANTS)

all: $(VARIANTS)

# Per-variant rules dispatched via sub-make so VARIANT scopes file paths.
$(VARIANTS):
	$(MAKE) -f $(DIR)Makefile VARIANT=$@ build

ifdef VARIANT

VARIANT_DIR := $(DIR)$(VARIANT)
TAR         := $(VARIANT_DIR)/supervisor.tar
HEADER      := $(VARIANT_DIR)/supervisor.h
INPUTS      := app.wasm manifest.json

.PHONY: build
build: $(HEADER)

$(TAR): $(addprefix $(VARIANT_DIR)/,$(INPUTS))
	tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
	    -C $(VARIANT_DIR) -cf $@ $(INPUTS)

$(HEADER): $(TAR)
	cd $(VARIANT_DIR) && xxd -i $(notdir $(TAR)) > $(notdir $(HEADER))
	sed -i 's/supervisor_tar/supervisor_wapp/g' $(HEADER)

endif

clean:
	rm -f $(addsuffix /supervisor.tar,$(addprefix $(DIR),$(VARIANTS)))
	rm -f $(addsuffix /supervisor.h,$(addprefix $(DIR),$(VARIANTS)))

# Aggregate builder for the sample wapp images.
#
# Each wapps/<name>/ carries its own Makefile and compiles <name>.wasm with the
# wasi-sdk clang, so this only fans out. Needs the wapp-sdk image (the engine
# build image has no wasm toolchain) — see the root Makefile's `wasm` target.
#
# wsh is excluded: it is the debug supervisor, packaged as a TAR by
# wasm/supervisor rather than staged as a plain wapp image. sheriff is a Zig
# submodule with no Makefile, so the wildcard never picks it up.

DIR   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
WAPPS := $(filter-out wsh,$(notdir $(patsubst %/,%,$(dir $(wildcard $(DIR)*/Makefile)))))

.PHONY: all clean $(WAPPS)

all: $(WAPPS)

$(WAPPS):
	$(MAKE) -C $(DIR)$@

clean:
	@for d in $(WAPPS); do $(MAKE) -C $(DIR)$$d clean; done

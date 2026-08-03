# Firmware-flasher wapp builder.
#
# The flasher is a Zig wapp built from the wapps/sheriff submodule, which
# produces flasher.wasm beside its supervisor image. Copied here so the
# registry-seed packaging finds it at the wapps/<name>/<name>.wasm path.
#
# ZIG is overridable — the wapp-sdk image's zig (default), or the submodule's
# own toolchain container:
#
#   make -C wapps/flasher ZIG='./scripts/dev zig'

DIR         := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SHERIFF_SRC := $(DIR)../sheriff
ZIG         ?= zig
WASM        := $(DIR)flasher.wasm
# The version the wasm was built with, for the registry ref it is seeded under.
# Written by the rule that produces the wasm, so the ref cannot name a version
# the image is not. registry-version.sh reduces the tree's description to what
# a registry version field accepts.
VERSION     := $(DIR)flasher.version

.PHONY: all clean

all: $(WASM) $(VERSION)

$(WASM) $(VERSION) &: $(wildcard $(SHERIFF_SRC)/src/*.zig) $(SHERIFF_SRC)/build.zig
	cd $(SHERIFF_SRC) && $(ZIG) build
	cp $(SHERIFF_SRC)/zig-out/bin/flasher.wasm $(WASM)
	$(DIR)registry-version.sh $(SHERIFF_SRC) > $(VERSION)

clean:
	rm -f $(WASM) $(VERSION)

# Provisioning-blob wapp builder.
#
# provisioning is a Zig wapp built from the wapps/sheriff submodule, which
# produces provisioning.wasm beside its supervisor image. Copied here so the
# registry-seed packaging finds it at the wapps/<name>/<name>.wasm path.
#
# ZIG is overridable — the wapp-sdk image's zig (default), or the submodule's
# own toolchain container:
#
#   make -C wapps/provisioning ZIG='./scripts/dev zig'

DIR         := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SHERIFF_SRC := $(DIR)../sheriff
ZIG         ?= zig
WASM        := $(DIR)provisioning.wasm
# The version the wasm was built with, for the ref it is seeded under. Written
# by the build rule, so the ref cannot name a version the image is not.
# registry-version.sh reduces the tree to what a registry version field accepts.
VERSION     := $(DIR)provisioning.version

.PHONY: all clean

all: $(WASM) $(VERSION)

# provisioning's own sources are prerequisites too: it is built from
# wapps/provisioning/ inside the submodule, and listing only src/ meant a
# change there produced no rebuild.
$(WASM) $(VERSION) &: $(wildcard $(SHERIFF_SRC)/src/*.zig) \
                      $(wildcard $(SHERIFF_SRC)/wapps/provisioning/*.zig) \
                      $(SHERIFF_SRC)/build.zig
	cd $(SHERIFF_SRC) && $(ZIG) build
	cp $(SHERIFF_SRC)/zig-out/bin/provisioning.wasm $(WASM)
	$(DIR)../registry-version.sh $(SHERIFF_SRC) > $(VERSION)

clean:
	rm -f $(WASM) $(VERSION)

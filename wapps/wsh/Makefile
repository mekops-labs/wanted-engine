mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))
NAME ?= $(current_dir)
WASM = $(NAME).wasm
HEADER = $(NAME).wasm.h
CLANG ?= /opt/wasi-sdk/bin/clang
VERSION = 0.5.0

# The wasi-sdk clang defaults to its own sysroot and compiler-rt for the
# wasm32-wasi target, so no explicit --sysroot / --rtlib is needed.
TARGET_FLAGS = --target=wasm32-wasi
CLANG_FLAGS = -Wl,-export-dynamic -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -z stack-size=4096 -Os -Wl,--strip-all -s $(TARGET_FLAGS) -DVERSION=\"$(VERSION)\"

SRCS := $(wildcard *.c)

all: $(WASM)

clean:
	rm -f *.o *.wasm

.PHONY: all clean

# Depend on the Makefile too: VERSION is compiled in via -DVERSION, so a version
# bump must force a rebuild even when no source changed.
$(WASM): $(SRCS) Makefile
	$(CLANG) $(CLANG_FLAGS) $(SRCS) -o $@


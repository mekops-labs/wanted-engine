# wlkload: Lookout load generator wapp, built with the wasi-sdk clang.

mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))
NAME ?= $(current_dir)
WASM = $(NAME).wasm
CLANG ?= /opt/wasi-sdk/bin/clang

# The wasi-sdk clang defaults to its own sysroot and compiler-rt for the
# wasm32-wasi target, so no explicit --sysroot / --rtlib is needed.
TARGET_FLAGS = --target=wasm32-wasi
# --compress-relocations: without it lld pads call_indirect's table index to
# 5 bytes, which the engine's WAMR build (reference types off) rejects.
CLANG_FLAGS = -Wl,-export-dynamic -Wl,--initial-memory=655360 -Wl,--max-memory=655360 -z stack-size=32768 -O2 -Wl,--strip-all -Wl,--compress-relocations -s $(TARGET_FLAGS)

SRCS := $(wildcard *.c)

all: $(WASM)

clean:
	rm -f *.o *.wasm

.PHONY: all clean

$(WASM): $(SRCS)
	$(CLANG) $(CLANG_FLAGS) $^ -o $@

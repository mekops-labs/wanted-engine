# hello sample wapp builder.
#
# Compiles the wapp source under this directory to <dir>.wasm with the
# wasi-sdk clang. Mirrors wapps/wsh/Makefile; the test/smoke-multiwapp.sh
# harness packages the resulting wasm + manifest.json into a registry .wapp.

mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))
NAME ?= $(current_dir)
WASM = $(NAME).wasm
CLANG ?= /opt/wasi-sdk/bin/clang

# The wasi-sdk clang defaults to its own sysroot and compiler-rt for the
# wasm32-wasi target, so no explicit --sysroot / --rtlib is needed.
TARGET_FLAGS = --target=wasm32-wasi
CLANG_FLAGS = -Wl,-export-dynamic -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -z stack-size=4096 -Os -Wl,--strip-all -s $(TARGET_FLAGS)

SRCS := $(wildcard *.c)

all: $(WASM)

clean:
	rm -f *.o *.wasm

.PHONY: all clean

$(WASM): $(SRCS)
	$(CLANG) $(CLANG_FLAGS) $^ -o $@

# bigmem test wapp builder.
#
# Mirrors wapps/membomb/Makefile but starts at one page and allows growth to
# eight (initial=64 KiB, max=512 KiB), so the wapp can try to grow past a 1-page
# engine cap. Used by test/memcap.sh to verify WASM_MAX_MEMORY_PAGES.

mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))
NAME ?= $(current_dir)
WASM = $(NAME).wasm
CLANG ?= /opt/wasi-sdk/bin/clang

TARGET_FLAGS = --target=wasm32-wasi
# --compress-relocations: without it lld pads call_indirect's table index to
# 5 bytes, which the engine's WAMR build (reference types off) rejects.
CLANG_FLAGS = -Wl,-export-dynamic -Wl,--initial-memory=65536 -Wl,--max-memory=524288 -z stack-size=4096 -Os -Wl,--strip-all -Wl,--compress-relocations -s $(TARGET_FLAGS)

SRCS := $(wildcard *.c)

all: $(WASM)

clean:
	rm -f *.o *.wasm

.PHONY: all clean

$(WASM): $(SRCS)
	$(CLANG) $(CLANG_FLAGS) $^ -o $@

NAME?=test_prog
SRC=$(NAME).c
WASM=$(NAME).wasm
HEADER=$(NAME).wasm.h
CLANG?=/opt/wasi-sdk/bin/clang

# The wasi-sdk clang defaults to its own sysroot and compiler-rt for
# wasm32-wasi, so the WASI build needs no explicit --sysroot / --rtlib.
# --compress-relocations: without it lld pads call_indirect's table index to
# 5 bytes, which the engine's WAMR build (reference types off) rejects.
CLANG_COMMON_FLAGS=-Wl,-export-dynamic -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -z stack-size=4096 -Os -Wl,--strip-all -Wl,--compress-relocations
ifeq (${WASI}, 1)
CLANG_FLAGS+=--target=wasm32-wasi $(CLANG_COMMON_FLAGS)
else
CLANG_FLAGS+=--target=wasm32 -nostdlib -Wl,--no-entry $(CLANG_COMMON_FLAGS)
endif

all: $(HEADER)

clean:
	rm -f *.wasm *.wasm.h

.PHONY: all clean

$(WASM): $(SRC)
	$(CLANG) $(CLANG_FLAGS) $< -o $@

$(HEADER): $(WASM)
	xxd -i $< >$@

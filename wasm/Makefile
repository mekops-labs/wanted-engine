NAME?=test_prog
SRC=$(NAME).c
WASM=$(NAME).wasm
HEADER=$(NAME).wasm.h
CLANG=clang
WASI_SYSROOT=/usr/share/wasi-sysroot

CLANG_COMMON_FLAGS=-Wl,-export-dynamic -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -z stack-size=4096 -Os -Wl,--strip-all
ifdef WASI
CLANG_FLAGS+=--target=wasm32-unknown-wasi --sysroot=$(WASI_SYSROOT) $(CLANG_COMMON_FLAGS)
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

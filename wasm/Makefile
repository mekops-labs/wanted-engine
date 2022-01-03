NAME=test_prog
SRC=$(NAME).c
WASM=$(NAME).wasm
HEADER=$(NAME).wasm.h
CLANG=clang
WASI_SYSROOT=/usr/share/wasi-sysroot

CLANG_COMMON_FLAGS=-Wl,-export-dynamic -Os -s
ifdef WASI
CLANG_FLAGS+=--target=wasm32-unknown-wasi --sysroot=$(WASI_SYSROOT) $(CLANG_COMMON_FLAGS)
else
CLANG_FLAGS+=--target=wasm32 -nostdlib -Wl,--no-entry $(CLANG_COMMON_FLAGS)
endif

all: $(HEADER)

clean:
	rm -f $(HEADER) $(WASM)

.PHONY: all clean

$(WASM): $(SRC)
	$(CLANG) $(CLANG_FLAGS) $< -o $@

$(HEADER): $(WASM)
	xxd -i $< >$@

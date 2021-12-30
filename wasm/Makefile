NAME=test_prog
SRC=$(NAME).c
WASM=$(NAME).wasm
HEADER=$(NAME).wasm.h
#CLANG_FLAGS=--target=wasm32-unknown-wasi --sysroot=/home/mek/work/nanocont/wasi-libc/sysroot -Os -s
CLANG_FLAGS=--target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-all -Os -s

all: $(HEADER)

clean:
	rm -f $(HEADER) $(WASM)

.PHONY: all clean

$(WASM): $(SRC)
	clang-11 $(CLANG_FLAGS) $< -o $@

$(HEADER): $(WASM)
	xxd -i $< >$@

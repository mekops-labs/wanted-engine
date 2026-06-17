# biginit test wapp builder.
#
# Declares FOUR initial linear-memory pages (256 KiB) so the module's starting
# memory exceeds a small engine cap. Used by test/memcap.sh to verify the
# load-time rejection in WASM_MAX_MEMORY_PAGES enforcement.

mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))
NAME ?= $(current_dir)
WASM = $(NAME).wasm
CLANG ?= /opt/wasi-sdk/bin/clang

TARGET_FLAGS = --target=wasm32-wasi
CLANG_FLAGS = -Wl,-export-dynamic -Wl,--initial-memory=262144 -Wl,--max-memory=524288 -z stack-size=4096 -Os -Wl,--strip-all -s $(TARGET_FLAGS)

SRCS := $(wildcard *.c)

all: $(WASM)

clean:
	rm -f *.o *.wasm

.PHONY: all clean

$(WASM): $(SRCS)
	$(CLANG) $(CLANG_FLAGS) $^ -o $@

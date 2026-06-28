# WANTED Engine — living build/test commands.
#
# All real work runs inside the standardized build container (see docker/), so
# the host only needs a container runtime. These targets are the canonical
# commands; the README points here rather than duplicating them.
#
# Override the runtime, image, or build dir as needed:
#   make test RUNNER=docker
#   make build IMAGE=localhost/wanted-build:dev
#   make build BUILD_DIR=build-dbg

RUNNER    ?= podman
IMAGE     ?= registry.gitlab.com/mekops/wanted/wanted-engine/build
BUILD_DIR ?= build
WSH_TAR   := ./wasm/supervisor/wsh/supervisor.tar

# Optional resource-limit profile (cmake/profiles/<name>.cmake).
PROFILE ?=
ifneq ($(PROFILE),)
PROFILE_CMAKE_ARG := -C /src/cmake/profiles/$(PROFILE).cmake
endif

# `make just <recipe> [args...]` forwards to the in-container Justfile (lint,
# static-analysis, and security recipes). The trailing words are the recipe and
# its arguments; stub them as no-op goals so make does not treat them as targets.
ifeq (just,$(firstword $(MAKECMDGOALS)))
JUST_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
$(eval $(JUST_ARGS):;@:)
endif

# Run a shell command inside the build container with the repo at /src.
RUN = $(RUNNER) run --rm -v "$(CURDIR):/src:Z" --entrypoint=/bin/sh $(IMAGE) -c

# --- NuttX simulator ------------------------------------------------------
# nuttx + nuttx-apps are shallow git submodules under third_party/ (the mekops
# forks). Everything builds under the engine (third_party/ + build-nuttx/), so
# the standard $(RUN) mount at /src is all that's needed. The build/test recipe
# lives in test/nuttx-sim.sh (shared with CI).

.DEFAULT_GOAL := help

.PHONY: all supervisor wapps build wsh test smoke-engine shell clean help \
        nuttx-deps nuttx-build nuttx-selftest nuttx-syscontrol nuttx-shell wsh-shell selftest \
        esp32 esp32-flash docs-sync just sizes memcap

all: build test ## build the engine and run the test suite

supervisor: ## compile the supervisor TAR images (wsh from wapps/wsh/ source)
	$(RUN) 'make -C /src/wasm/supervisor'

wapps: ## compile the sample wapp images under wapps/ (excludes the wsh supervisor, built by `supervisor`)
	$(RUN) 'set -e; for d in /src/wapps/*/; do if [ "$$d" = /src/wapps/wsh/ ]; then continue; fi; if [ -f "$${d}Makefile" ]; then make -C "$$d"; fi; done'

build: supervisor ## build the engine + CLI with the production (sheriff) supervisor [PROFILE=tiny|constrained|small|big]
	$(RUN) 'mkdir -p /src/$(BUILD_DIR) && cd /src/$(BUILD_DIR) && cmake -G Ninja $(PROFILE_CMAKE_ARG) /src && ninja'

wsh: supervisor ## build the engine + CLI with the wsh debug supervisor compiled in [PROFILE=...]
	$(RUN) 'mkdir -p /src/$(BUILD_DIR) && cd /src/$(BUILD_DIR) && cmake -G Ninja $(PROFILE_CMAKE_ARG) /src -DWANTED_SUPERVISOR_IMAGE_PATH=$(WSH_TAR) && ninja'

test: ## run the unit + smoke suite via ctest
	$(RUN) 'cd /src/$(BUILD_DIR) && ctest --output-on-failure'

smoke-engine: ## boot the production supervisor and assert a clean instantiate
	$(RUN) 'cd /src && ./test/smoke-engine.sh ./$(BUILD_DIR)/cmd/wanted-cli ./test/smoke-engine-config.json'

selftest: build ## run the in-WASM selftest suite + system-control checks on Linux
	$(RUN) 'cd /src && ./test/selftest.sh ./$(BUILD_DIR)/cmd/wanted-cli ./test/selftest-config.json \
	    && ./test/syscontrol.sh ./$(BUILD_DIR)/cmd/wanted-cli ./configs/example_config_wsh.json'

wsh-shell: wsh ## build wsh and open the interactive wsh prompt on Linux (wanted-cli)
	$(RUNNER) run --rm -it -v "$(CURDIR):/src:Z" --entrypoint=/bin/sh $(IMAGE) -c \
	    'cd /src && ./$(BUILD_DIR)/cmd/wanted-cli ./configs/example_config_wsh.json'

# The recipe lives in test/nuttx-sim.sh so the Makefile (which wraps it in the
# build container) and GitLab CI (already inside it) share one source of truth.
nuttx-deps: ## link the engine app package into the checked-out nuttx-apps submodule
	$(RUN) 'cd /src && ./test/nuttx-sim.sh deps'

nuttx-build: supervisor ## configure + build the NuttX sim (wsh as init over hostfs) [PROFILE=tiny|constrained|small|big]
	$(RUN) 'cd /src && PROFILE=$(PROFILE) ./test/nuttx-sim.sh deps build'

nuttx-selftest: supervisor ## run the in-WASM selftest suite + system-control checks on the NuttX sim
	$(RUN) 'cd /src && ./test/nuttx-sim.sh selftest syscontrol'

nuttx-syscontrol: supervisor ## run the system-control (poweroff/reboot/exit) checks on the NuttX sim
	$(RUN) 'cd /src && ./test/nuttx-sim.sh syscontrol'

nuttx-shell: nuttx-build ## build the wsh sim and drop into the interactive wsh prompt
	$(RUNNER) run --rm -it -v "$(CURDIR):/src:Z" -w /src/build-nuttx/simroot \
	    --entrypoint=/bin/sh $(IMAGE) -c '/src/third_party/nuttx/nuttx'

# --- ESP32 firmware (real hardware) ---------------------------------------
# Cross-build the NuttX firmware for the Waveshare ESP32 One in the xtensa
# toolchain container (distinct from the sim's $(IMAGE)). The board config is the
# `wanted` variant of esp32-devkitc in the nuttx fork; the engine app is linked
# in by `nuttx-sim.sh deps` (shared with the sim). The boot ROMFS bakes the wsh
# supervisor, so `supervisor` is a prerequisite. Output is
# third_party/nuttx/nuttx.bin, flashed at 0x1000 (ESP32 Simple Boot).
ESP32_IMAGE ?= localhost/wanted-esp32
ESP32_PORT  ?= /dev/ttyUSB0
ESP32_BAUD  ?= 460800
# The xtensa image's entrypoint drops to a build user derived from /src's owner,
# which rootless bind-mount UID remapping defeats; bypass it and build as the
# container root (mapped back to the invoking host user under rootless podman).
ESP32_RUN = $(RUNNER) run --rm -v "$(CURDIR):/src:Z" --entrypoint=/bin/sh $(ESP32_IMAGE) -c

esp32: supervisor ## cross-build the ESP32 firmware -> third_party/nuttx/nuttx.bin
	$(RUN) 'cd /src && ./test/nuttx-sim.sh deps'
	$(ESP32_RUN) 'cd /src/third_party/nuttx && \
	    { [ -f .config ] || ./tools/configure.sh -a ../nuttx-apps esp32-devkitc:wanted; } && \
	    make -j"$$(nproc)"'

esp32-flash: ## flash third_party/nuttx/nuttx.bin to the board [ESP32_PORT=/dev/ttyUSB0]
	esptool.py -c esp32 -p $(ESP32_PORT) -b $(ESP32_BAUD) --before default_reset \
	    --after no_reset write_flash -fs detect -fm dio -ff 40m 0x1000 \
	    third_party/nuttx/nuttx.bin

just: ## run a Justfile recipe in the build container, e.g. make just lint-format
	$(RUN) 'cd /src && just $(JUST_ARGS)'

sizes: ## report per-wapp + engine memory footprint for each profile (linux + nuttx ABIs)
	$(RUN) 'cd /src && ./utils/measure-sizes.sh'

memcap: supervisor ## negative test: WASM_MAX_MEMORY_PAGES bounds a wapp's linear-memory growth
	$(RUN) 'cd /src && ./test/memcap.sh'

shell: ## open an interactive shell in the build container
	$(RUNNER) run --rm -it -v "$(CURDIR):/src:Z" --entrypoint="" $(IMAGE) bash

clean: ## remove every build artifact (Linux + NuttX sim + wasm/wapps + submodule objects)
	rm -rf $(CURDIR)/$(BUILD_DIR) $(CURDIR)/build-nuttx $(CURDIR)/registry
	# Only wsh/selftest app.wasm are generated from wapps/ source; sheriff's is a
	# committed blob, so never delete it.
	rm -f $(CURDIR)/wasm/*.wasm* $(CURDIR)/wasm/supervisor/*/supervisor.tar \
	      $(CURDIR)/wasm/supervisor/wsh/app.wasm \
	      $(CURDIR)/wasm/supervisor/selftest/app.wasm
	rm -f $(CURDIR)/wapps/*/*.wasm $(CURDIR)/wapps/*/*.wasm.h $(CURDIR)/wapps/*/*.o
	# NuttX kernel objects + .config live in the submodule tree; an incremental
	# rebuild over a stale tree silently runs old code, so distclean it too.
	$(RUN) 'cd /src && ./test/nuttx-sim.sh clean'

# docs-sync runs on the host, not in the build container: it only copies Markdown
# (no toolchain needed) to the destination directory.
# Pass the target content dir as DOCS_DEST.
docs-sync: ## sync docs/*.md to the MekOps Hugo blog (pass DOCS_DEST=<blog content dir>)
	@test -n "$(DOCS_DEST)" || { echo "DOCS_DEST is required, e.g. make docs-sync DOCS_DEST=<path to blog>/content/projects/wanted"; exit 1; }
	rsync -av --include='*.md' --exclude='*' docs/ $(DOCS_DEST)/

help: ## list the available targets
	@grep -hE '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) \
	    | awk 'BEGIN{FS=":.*## "}{printf "  make %-14s %s\n", $$1, $$2}'

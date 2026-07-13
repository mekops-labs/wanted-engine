# WANTED Engine — host wrapper.
#
# This Makefile is a thin wrapper: it runs the canonical `just` recipes inside
# the standardized build container. Every build/test/lint recipe lives in the
# Justfile (run `make help`, or `just --list`). Any goal not handled below is
# forwarded verbatim to `just` in the container, so `make build` == `just build`
# run in the image. Inside the devcontainer or CI — already in the build
# environment — call `just <recipe>` directly and skip this wrapper entirely.
#
# Override the runtime, image, build dir, or profile as needed:
#   make test RUNNER=docker
#   make build IMAGE=localhost/wanted-build:dev
#   make build BUILD_DIR=build-dbg PROFILE=tiny

RUNNER    ?= podman
IMAGE     ?= registry.gitlab.com/mekops/wanted/wanted-engine/build
BUILD_DIR ?= build
PROFILE   ?=

# Forward an override into the container only when the user actually set it (env
# or command line) — never make's built-in default (e.g. CC defaults to `cc`).
fwd = $(if $(filter-out undefined default,$(origin $(1))),-e $(1)=$($(1)))

# Env handed to the in-container `just` so recipes pick up overrides. BUILD_DIR
# and PROFILE always flow; the rest only when explicitly set (mirrors the
# variables CI sets per job).
ENVS = -e BUILD_DIR=$(BUILD_DIR) -e PROFILE=$(PROFILE) \
       $(call fwd,CC) $(call fwd,CMAKE_EXTRA_ARGS) \
       $(call fwd,NUTTX_SKIP_BUILD) $(call fwd,NUTTX_CLEAN)

# Run a `just` recipe inside the build container with the repo mounted at /src.
# --entrypoint=just bypasses the image's user-remapping entrypoint, as the
# interactive targets below already do — fine under rootless podman.
JUST = $(RUNNER) run --rm -v "$(CURDIR):/src:Z" -w /src $(ENVS) --entrypoint=just $(IMAGE)

.DEFAULT_GOAL := help

.PHONY: help shell wsh-shell nuttx-shell esp32 esp32-flash rp2350 rp2350-flash rp2350-flash-swd rp2350-reset rp2350-sign docs-sync FORCE

# Catch-all: forward any goal without an explicit rule below to `just` in the
# container. FORCE defeats make's "up to date" check so a goal that matches an
# existing path (e.g. the build/ dir) still runs.
%: FORCE
	@$(JUST) $@

FORCE:

# Stop the catch-all pattern from "remaking" this makefile (make checks whether
# its own makefile needs rebuilding before running any goal).
Makefile: ;

# --- host / interactive targets (cannot be a plain in-container `just`) ----

shell: ## open an interactive shell in the build container
	$(RUNNER) run --rm -it -v "$(CURDIR):/src:Z" -w /src --entrypoint="" $(IMAGE) bash

wsh-shell: ## build wsh and open the interactive wsh prompt on Linux (wanted-cli)
	$(JUST) wsh
	$(RUNNER) run --rm -it -v "$(CURDIR):/src:Z" -w /src --entrypoint=/bin/sh $(IMAGE) -c \
	    './$(BUILD_DIR)/cmd/wanted-cli ./configs/example_config_wsh.json'

nuttx-shell: ## build the wsh sim and drop into the interactive wsh prompt
	$(JUST) nuttx-build
	$(RUNNER) run --rm -it -v "$(CURDIR):/src:Z" -w /src/build-nuttx/simroot \
	    --entrypoint=/bin/sh $(IMAGE) -c '/src/third_party/nuttx/nuttx'

# ESP32 cross-build uses a distinct xtensa toolchain image; the supervisor +
# nuttx-deps stage in the build image, then the firmware links in the xtensa one.
ESP32_IMAGE ?= localhost/wanted-esp32
ESP32_PORT  ?= /dev/ttyUSB0
ESP32_BAUD  ?= 460800
# As with $(JUST), bypass the xtensa image entrypoint (its build-user remap
# collides with rootless bind-mount UID mapping); build as container root,
# mapped back to the invoking host user under rootless podman.
ESP32_JUST = $(RUNNER) run --rm -v "$(CURDIR):/src:Z" -w /src --entrypoint=just $(ESP32_IMAGE)

esp32: ## cross-build the ESP32 firmware -> third_party/nuttx/nuttx.bin
	$(JUST) supervisor
	$(JUST) nuttx-deps
	$(ESP32_JUST) esp32-build

esp32-flash: ## flash third_party/nuttx/nuttx.bin to the board [ESP32_PORT=/dev/ttyUSB0]
	esptool.py -c esp32 -p $(ESP32_PORT) -b $(ESP32_BAUD) --before default_reset \
	    --after no_reset write_flash -fs detect -fm dio -ff 40m 0x1000 \
	    third_party/nuttx/nuttx.bin

# --- RP2350 firmware (real hardware) --------------------------------------
# Cross-builds the NuttX firmware for the Adafruit Feather RP2350 in the ARM
# Cortex-M33 toolchain container ($(RP2350_IMAGE), from
# docker/Containerfile.rp2350; distinct from the sim's $(IMAGE)). The engine app
# is linked into nuttx-apps by `nuttx-sim.sh deps` (shared with the sim); the
# boot ROMFS bakes the wsh supervisor, so `supervisor` is a prerequisite. The
# rp23xx board POSTBUILD runs `picotool uf2 convert` -> third_party/nuttx/nuttx.uf2.
#
# WANTED board variants:
#   make rp2350 RP2350_CONFIG=adafruit-feather-rp2350:wanted            # wsh supervisor
#   make rp2350 RP2350_CONFIG=adafruit-feather-rp2350:sheriff PROFILE=small  # Sheriff over USB-CDC
# The `sheriff` config puts the console on UART0 (Debug Probe) and frees the
# native USB-CDC for the Sheriff<->Deputy link; flash it over SWD with
# `rp2350-flash-swd` and watch the console on the Probe's UART.
RP2350_IMAGE  ?= localhost/wanted-rp2350
RP2350_CONFIG ?= raspberrypi-pico-2:usbnsh
RP2350_BIN    ?= third_party/nuttx/nuttx.uf2
# The xtensa image bypasses its UID-remap entrypoint under rootless podman; do
# the same here and build as the container root (mapped back to the host user).
RP2350_RUN = $(RUNNER) run --rm -v "$(CURDIR):/src:Z" --entrypoint=/bin/sh $(RP2350_IMAGE) -c

# openocd (RPi fork, shipped in $(RP2350_IMAGE)) driving the board over SWD via a
# Raspberry Pi Debug Probe (CMSIS-DAP). Needs raw USB access, hence --privileged
# + /dev/bus/usb; the repo is mounted so `program` can read the ELF. Flashes or
# resets a *running* board over SWD, no BOOTSEL needed (the ELF, not the .uf2,
# is what openocd flashes).
RP2350_ELF     ?= third_party/nuttx/nuttx
RP2350_OPENOCD = $(RUNNER) run --rm --privileged -v /dev/bus/usb:/dev/bus/usb \
    -v "$(CURDIR):/src:Z" --entrypoint=/usr/local/bin/openocd $(RP2350_IMAGE) \
    -f interface/cmsis-dap.cfg -c 'transport select swd' -f target/rp2350.cfg \
    -c 'adapter speed 5000'

rp2350: supervisor ## cross-build the RP2350 firmware -> third_party/nuttx/nuttx.uf2 [RP2350_CONFIG=... PROFILE=...]
	$(RP2350_RUN) 'cd /src && ./test/nuttx-sim.sh deps && cd third_party/nuttx && \
	    { [ -f .config ] || ./tools/configure.sh -a ../nuttx-apps $(RP2350_CONFIG); } && \
	    DEFS=""; \
	    if [ -n "$(PROFILE)" ]; then \
	      f=/src/cmake/profiles/$(PROFILE).cmake; \
	      [ -f "$$f" ] || { echo "unknown profile '$(PROFILE)' (no $$f)" >&2; exit 1; }; \
	      DEFS=$$(sed -nE "s/^[[:space:]]*set\(([A-Z_]+)[[:space:]]+([0-9]+).*/-D\1=\2/p" "$$f" | tr "\n" " "); \
	    fi; \
	    make -j"$$(nproc)" WANTED_RESOURCE_DEFINES="$$DEFS"'

rp2350-flash: ## flash $(RP2350_BIN) over USB; put board in BOOTSEL first (hold BOOTSEL, tap RESET) [RP2350_BIN=...]
	picotool load -x $(RP2350_BIN)

rp2350-flash-swd: ## flash $(RP2350_ELF) over SWD via a Raspberry Pi Debug Probe — no BOOTSEL needed [RP2350_ELF=...]
	$(RP2350_OPENOCD) -c 'program /src/$(RP2350_ELF) verify reset exit'

rp2350-reset: ## reset the running board over SWD via a Raspberry Pi Debug Probe
	$(RP2350_OPENOCD) -c 'init; reset run; exit'

rp2350-sign: ## sign $(RP2350_BIN) and validate the signature offline (no OTP, no device) [RP2350_BIN=...]
	$(RP2350_RUN) './test/rp2350-sign-verify.sh $(RP2350_BIN)'

# docs-sync runs on the host, not in the build container: it only copies Markdown
# (no toolchain needed) to the destination directory. Pass DOCS_DEST.
docs-sync: ## sync docs/*.md to the MekOps Hugo blog (pass DOCS_DEST=<blog content dir>)
	@test -n "$(DOCS_DEST)" || { echo "DOCS_DEST is required, e.g. make docs-sync DOCS_DEST=<path to blog>/content/projects/wanted"; exit 1; }
	rsync -av --include='*.md' --exclude='*' docs/ $(DOCS_DEST)/

help: ## list the available targets
	@echo "Host wrapper targets (run on the host):"
	@grep -hE '^[a-zA-Z0-9_-]+:.*## ' $(MAKEFILE_LIST) \
	    | awk 'BEGIN{FS=":.*## "}{printf "  make %-18s %s\n", $$1, $$2}'
	@echo
	@echo "Container recipes (forwarded to just; also: just <recipe> in the devcontainer):"
	@$(JUST) --list

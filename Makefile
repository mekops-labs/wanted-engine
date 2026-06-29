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

.PHONY: help shell wsh-shell nuttx-shell esp32 esp32-flash docs-sync FORCE

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

# docs-sync runs on the host, not in the build container: it only copies Markdown
# (no toolchain needed) to the destination directory. Pass DOCS_DEST.
docs-sync: ## sync docs/*.md to the MekOps Hugo blog (pass DOCS_DEST=<blog content dir>)
	@test -n "$(DOCS_DEST)" || { echo "DOCS_DEST is required, e.g. make docs-sync DOCS_DEST=<path to blog>/content/projects/wanted"; exit 1; }
	rsync -av --include='*.md' --exclude='*' docs/ $(DOCS_DEST)/

help: ## list the available targets
	@echo "Host wrapper targets (run on the host):"
	@grep -hE '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) \
	    | awk 'BEGIN{FS=":.*## "}{printf "  make %-12s %s\n", $$1, $$2}'
	@echo
	@echo "Container recipes (forwarded to just; also: just <recipe> in the devcontainer):"
	@$(JUST) --list

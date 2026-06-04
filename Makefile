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
IMAGE     ?= registry.gitlab.com/wanted-project/wanted-engine/build
BUILD_DIR ?= build
WSH_TAR   := ./wasm/supervisor/wsh/supervisor.tar

# Run a shell command inside the build container with the repo at /src.
RUN = $(RUNNER) run --rm -v "$(CURDIR):/src:Z" --entrypoint=/bin/sh $(IMAGE) -c

.DEFAULT_GOAL := help

.PHONY: all supervisor wapps build wsh test smoke smoke-engine smoke-multiwapp shell clean help

all: build test ## build the engine and run the test suite

supervisor: ## compile the supervisor TAR images (wsh from wapps/wsh/ source)
	$(RUN) 'make -C /src/wasm/supervisor'

wapps: ## compile the sample wapp images under wapps/ (excludes the wsh supervisor, built by `supervisor`)
	$(RUN) 'set -e; for d in /src/wapps/*/; do if [ "$$d" = /src/wapps/wsh/ ]; then continue; fi; if [ -f "$${d}Makefile" ]; then make -C "$$d"; fi; done'

build: supervisor ## build the engine + CLI with the production (sheriff) supervisor
	$(RUN) 'mkdir -p /src/$(BUILD_DIR) && cd /src/$(BUILD_DIR) && cmake -G Ninja /src && ninja'

wsh: supervisor ## build the engine + CLI with the wsh debug supervisor compiled in
	$(RUN) 'mkdir -p /src/$(BUILD_DIR) && cd /src/$(BUILD_DIR) && cmake -G Ninja /src -DWANTED_SUPERVISOR_IMAGE_PATH=$(WSH_TAR) && ninja'

test: ## run the unit + smoke suite via ctest
	$(RUN) 'cd /src/$(BUILD_DIR) && ctest --output-on-failure'

smoke: ## run the VFS/control-plane smoke tests through the wsh supervisor
	$(RUN) 'cd /src && ./test/smoke.sh ./$(BUILD_DIR)/cmd/wanted-cli ./docs/example_config_smoke.json'

smoke-engine: ## boot the production supervisor and assert a clean instantiate
	$(RUN) 'cd /src && ./test/smoke-engine.sh ./$(BUILD_DIR)/cmd/wanted-cli ./test/smoke-engine-config.json'

smoke-multiwapp: ## drive wsh to launch a sample wapp; assert it runs concurrently with the supervisor
	$(RUN) 'cd /src && ./test/smoke-multiwapp.sh ./$(BUILD_DIR)/cmd/wanted-cli ./docs/example_config_smoke.json'

shell: ## open an interactive shell in the build container
	$(RUNNER) run --rm -it -v "$(CURDIR):/src:Z" --entrypoint="" $(IMAGE) bash

clean: ## remove the build directory
	rm -rf $(CURDIR)/$(BUILD_DIR)

help: ## list the available targets
	@grep -hE '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) \
	    | awk 'BEGIN{FS=":.*## "}{printf "  make %-14s %s\n", $$1, $$2}'

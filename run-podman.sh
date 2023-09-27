#!/bin/sh

podman run --rm -it -v $PWD:/src:Z --entrypoint="" registry.gitlab.com/wanted-project/wanted-engine/build bash

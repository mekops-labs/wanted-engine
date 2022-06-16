#!/bin/sh

docker run --rm -it -v $PWD:/src registry.gitlab.com/wanted-project/wanted-engine/build bash

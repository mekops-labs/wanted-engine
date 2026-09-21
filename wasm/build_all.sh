#!/bin/bash
# SPDX-License-Identifier: Apache-2.0

for i in *.c; do
	name=${i%.c}
	wasi=0
	echo "${name}" | grep -q wasi && {
		wasi=1
	}
	echo "Building ${name} (WASI=${wasi})..."
	make NAME="${name}" WASI=${wasi}
done

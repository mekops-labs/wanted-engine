#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Rebuilds the layered image fixtures with an unmodified `podman build` and
# keeps each uncompressed layer, topmost last, with the digest list of the
# image config. Usage: test/fixtures/layers/make-fixtures.sh
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -r "$work"' EXIT

# Builds `tag` from the Containerfile in `ctx`, then writes its layers to
# "$here/<name>/layer-<n>.tar" in manifest order, base first.
export_layers() {
    local name=$1 tag=$2 ctx=$3

    podman build -q -t "$tag" "$ctx" >/dev/null
    podman save --format oci-archive "$tag" -o "$work/$name.tar"
    mkdir -p "$work/$name"
    tar -xf "$work/$name.tar" -C "$work/$name"
    mkdir -p "$here/$name"
    python3 - "$work/$name" "$here/$name" <<'PY'
import gzip
import json
import sys

root, out = sys.argv[1], sys.argv[2]


def blob(digest):
    return f"{root}/blobs/sha256/{digest.split(':')[1]}"


index = json.load(open(f"{root}/index.json"))
manifest = json.load(open(blob(index["manifests"][0]["digest"])))
config = json.load(open(blob(manifest["config"]["digest"])))
with open(f"{out}/diff_ids.txt", "w") as ids:
    for n, layer in enumerate(manifest["layers"]):
        with open(f"{out}/layer-{n}.tar", "wb") as f:
            f.write(gzip.open(blob(layer["digest"])).read())
    for diff in config["rootfs"]["diff_ids"]:
        ids.write(diff.split(":")[1] + "\n")
PY
}

# Three layers from three COPY steps, with a directory and a file in each.
mkdir -p "$work/plain/data/sub"
printf 'wasm-code' >"$work/plain/app.wasm"
printf 'one' >"$work/plain/data/one.txt"
printf 'two' >"$work/plain/data/sub/two.txt"
printf 'cfg' >"$work/plain/config.json"
cat >"$work/plain/Containerfile" <<'EOT'
FROM scratch
COPY app.wasm /app.wasm
COPY data /data
COPY config.json /config.json
EOT
export_layers plain localhost/layer-fixture:plain "$work/plain"

# A middle layer that carries a file whiteout and an opaque directory, as the
# layer of an image does that deletes a file and replaces a directory.
mkdir -p "$work/mask/base/data/sub" "$work/mask/base/etc" \
    "$work/mask/mid/data" "$work/mask/mid/etc" "$work/mask/top"
printf 'wasm' >"$work/mask/base/app.wasm"
printf 'a' >"$work/mask/base/data/a.txt"
printf 'b' >"$work/mask/base/data/sub/b.txt"
printf 'm' >"$work/mask/base/etc/motd"
: >"$work/mask/mid/etc/.wh.motd"
printf 'n' >"$work/mask/mid/new.txt"
: >"$work/mask/mid/data/.wh..wh..opq"
printf 'fresh' >"$work/mask/mid/data/fresh.txt"
printf 'top' >"$work/mask/top/top.txt"
cat >"$work/mask/Containerfile" <<'EOT'
FROM scratch
COPY base/ /
COPY mid/ /
COPY top/ /
EOT
export_layers mask localhost/layer-fixture:mask "$work/mask"

podman rmi -f localhost/layer-fixture:plain localhost/layer-fixture:mask >/dev/null
echo "fixtures written to $here"

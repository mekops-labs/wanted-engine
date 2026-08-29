#!/bin/bash
# Build the toolchain images as multi-arch manifest lists and push them; CI has
# no Docker-in-Docker and only pulls them. Each version tag comes from the
# Containerfile's own `LABEL version=`, so it cannot drift. The `firmware`
# image is a built board binary instead: single-arch, one layer, rendered from
# docker/Containerfile.firmware.in per build.
#
# Usage: docker/publish-images.sh [-a AUTHFILE] [-b BOARD -i BIN] [image ...]
#   -a AUTHFILE   push (podman --authfile); omitted, only build + verify.
#   -b BOARD      board name the firmware image is published under.
#   -i BIN        path to the built firmware .bin.
#   -c VARIANT    configuration name, tagged <release>-<variant>.
#
#   docker/publish-images.sh                          # build + verify, no push
#   docker/publish-images.sh -a ~/auth.json           # ... and push both
#   docker/publish-images.sh -a ~/auth.json wapp-sdk  # just the wapp SDK image
#   docker/publish-images.sh -a ~/auth.json -b rp2350 -i wanted.bin firmware
#   docker/publish-images.sh -b rp2350 -i wanted.bin -c nowifi firmware
set -euo pipefail

REGISTRY=${REGISTRY:-registry.gitlab.com/mekops/wanted/wanted-engine}
PLATFORMS=${PLATFORMS:-linux/amd64,linux/arm64}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONTEXT=$ROOT/docker

usage() {
    echo "usage: docker/publish-images.sh [-a AUTHFILE] [-b BOARD -i BIN] [build|wapp-sdk|firmware ...]" >&2
    exit 2
}

authfile=
board=
bin=
variant=
while getopts ':a:b:i:c:h' opt; do
    case $opt in
        a) authfile=$OPTARG ;;
        b) board=$OPTARG ;;
        i) bin=$OPTARG ;;
        c) variant=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

images=("$@")
if [ ${#images[@]} -eq 0 ]; then
    images=(build wapp-sdk)
fi

if [ -n "$authfile" ] && [ ! -f "$authfile" ]; then
    echo "FAIL: authfile not found: $authfile" >&2
    exit 1
fi

# Map an image name to its Containerfile, relative to the repo root.
containerfile_for() {
    case $1 in
        build)    echo "docker/Dockerfile" ;;
        wapp-sdk) echo "docker/Containerfile.wapp-sdk" ;;
        *)        echo "FAIL: unknown image '$1' (want: build, wapp-sdk, firmware)" >&2; return 1 ;;
    esac
}

# The version a firmware image publishes under is the release tag: off a tag
# the build stamps `X.Y.Z+<timestamp>`, which no device would ever match, so
# only a tagged, clean tree can publish.
firmware_version() {
    local tag
    if ! tag=$(git -C "$ROOT" describe --tags --exact-match 2>/dev/null); then
        echo "FAIL: HEAD is not tagged; a firmware image publishes under a release tag" >&2
        return 1
    fi
    if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
        echo "FAIL: working tree is dirty; the .bin would not match the tag it publishes under" >&2
        return 1
    fi
    echo "${tag#v}"
}

# The tag a firmware image publishes under. A variant names the build's
# config, so two builds of one release never share a tag; it goes after the
# release, never inside it, since that is what convergence judges.
firmware_tag() {
    local release=$1
    if [ -z "$variant" ]; then
        echo "$release"
        return 0
    fi
    if ! [[ $variant =~ ^[A-Za-z0-9._]+$ ]]; then
        echo "FAIL: variant '$variant' must match [A-Za-z0-9._]+; a '-' or '+' would move the release core" >&2
        return 1
    fi
    echo "$release-$variant"
}

# The configuration the binary was built from, so the image records which one
# a variant name stands for. Empty where no build directory is at hand.
config_hash() {
    local cfg
    for cfg in "$ROOT/build/.config" "$(dirname "$1")/.config"; do
        if [ -f "$cfg" ]; then
            sha256sum "$cfg" | cut -d' ' -f1
            return 0
        fi
    done
    echo ""
}

# Render the firmware Containerfile for one .bin. The digest and size come from
# the artifact itself, so `version_of` reads the version back out of the render
# exactly as it does for a hand-written Containerfile.
render_firmware() {
    local ver=$1 image_bin=$2 out=$3
    sed -e "s|@VERSION@|$ver|" \
        -e "s|@DIGEST@|sha256:$(sha256sum "$image_bin" | cut -d" " -f1)|" \
        -e "s|@SIZE@|$(stat -c %s "$image_bin")|" \
        -e "s|@CONFIG@|$(config_hash "$image_bin")|" \
        "$ROOT/docker/Containerfile.firmware.in" > "$out"
}

# One layer, a firmware.digest label that still matches the source .bin, and a
# tag whose release core is the release itself.
verify_firmware() {
    local image=$1 image_bin=$2 release=$3 layers labelled actual tagged core
    layers=$(podman image inspect --format '{{len .RootFS.Layers}}' "$image")
    if [ "$layers" != 1 ]; then
        echo "FAIL: $image has $layers layers, want 1" >&2
        return 1
    fi
    labelled=$(podman image inspect --format '{{index .Config.Labels "firmware.digest"}}' "$image")
    actual="sha256:$(sha256sum "$image_bin" | cut -d' ' -f1)"
    if [ "$labelled" != "$actual" ]; then
        echo "FAIL: $image labels $labelled, .bin hashes to $actual" >&2
        return 1
    fi
    tagged=$(podman image inspect --format '{{index .Config.Labels "version"}}' "$image")
    core=${tagged%%-*}
    if [ "$core" != "$release" ]; then
        echo "FAIL: $image tags release '$core', built from '$release'" >&2
        return 1
    fi
    echo "  one layer, release $core, firmware.digest $labelled"
}

# Build and push one board's firmware image: single-arch, so none of the
# manifest-list machinery applies.
publish_firmware() {
    local release ver cf image
    if [ -z "$board" ] || [ -z "$bin" ]; then
        echo "FAIL: firmware needs -b BOARD and -i BIN" >&2
        return 1
    fi
    if [ ! -f "$bin" ]; then
        echo "FAIL: firmware image not found: $bin" >&2
        return 1
    fi
    bin=$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")

    release=$(firmware_version)
    ver=$(firmware_tag "$release")
    cf=$(mktemp)
    trap 'rm -f "$cf"' RETURN
    render_firmware "$ver" "$bin" "$cf"
    ver=$(version_of "$cf")
    image=$REGISTRY/firmware/$board:$ver

    echo "==> building $image"
    podman build -t "$image" --build-arg "BIN=$(basename "$bin")" -f "$cf" "$(dirname "$bin")"

    echo "==> verifying $image"
    verify_firmware "$image" "$bin" "$release"

    if [ -z "$authfile" ]; then
        echo "==> not pushing $image (no -a AUTHFILE)"
        return 0
    fi
    echo "==> pushing $image"
    podman push --authfile "$authfile" "$image"
}

# The image's own LABEL is the single source of truth for its version tag.
version_of() {
    local ver
    ver=$(sed -n 's/^LABEL version="\(.*\)"$/\1/p' "$1" | head -1)
    if [ -z "$ver" ]; then
        echo "FAIL: no 'LABEL version=\"...\"' in $1" >&2
        return 1
    fi
    echo "$ver"
}

# Assert the built tag really is a manifest list covering every requested
# platform, and that each leg runs and reports the arch it claims.
verify() {
    local image=$1 arches arch uname_m
    arches=$(podman manifest inspect "$image" |
        sed -n 's/.*"architecture": "\([^"]*\)".*/\1/p' | sort -u | tr '\n' ' ')

    echo "  manifest list: ${arches% }"
    for platform in ${PLATFORMS//,/ }; do
        arch=${platform#*/}
        case " $arches " in
            *" $arch "*) ;;
            *) echo "FAIL: $image has no $arch leg" >&2; return 1 ;;
        esac
        uname_m=$(podman run --rm --platform "$platform" --entrypoint="" "$image" uname -m)
        echo "  $platform runs: $uname_m"
    done
}

for name in "${images[@]}"; do
    if [ "$name" = firmware ]; then
        publish_firmware
        continue
    fi

    cf=$(containerfile_for "$name")
    ver=$(version_of "$ROOT/$cf")
    image=$REGISTRY/$name:$ver

    echo "==> building $image ($PLATFORMS)"
    podman build --platform "$PLATFORMS" --manifest "$image" -f "$ROOT/$cf" "$CONTEXT"
    podman tag "$image" "$REGISTRY/$name:latest"

    echo "==> verifying $image"
    verify "$image"

    if [ -z "$authfile" ]; then
        echo "==> not pushing $image (no -a AUTHFILE)"
        continue
    fi

    for tag in "$ver" latest; do
        echo "==> pushing $REGISTRY/$name:$tag"
        podman manifest push --all --authfile "$authfile" "$REGISTRY/$name:$tag"
    done
done

echo "done"

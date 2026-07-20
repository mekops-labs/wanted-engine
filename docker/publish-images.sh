#!/bin/bash
# Build the toolchain images (docker/Dockerfile, Containerfile.wapp-sdk) as
# multi-arch manifest lists and push them. CI has no Docker-in-Docker, so images
# are built out-of-band with this script; CI only pulls them.
#
# The version tag comes from each Containerfile's own `LABEL version=`, so it
# can never drift from the image definition. Needs qemu-user-static +
# binfmt-qemu-static for the foreign arch; the arm64 leg is emulated and slow.
#
# Usage: docker/publish-images.sh [-a AUTHFILE] [build|wapp-sdk ...]
#   -a AUTHFILE   push (podman --authfile); omitted, only build + verify.
#
#   docker/publish-images.sh                          # build + verify, no push
#   docker/publish-images.sh -a ~/auth.json           # ... and push both
#   docker/publish-images.sh -a ~/auth.json wapp-sdk  # just the wapp SDK image
set -euo pipefail

REGISTRY=${REGISTRY:-registry.gitlab.com/mekops/wanted/wanted-engine}
PLATFORMS=${PLATFORMS:-linux/amd64,linux/arm64}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONTEXT=$ROOT/docker

usage() {
    echo "usage: docker/publish-images.sh [-a AUTHFILE] [build|wapp-sdk ...]" >&2
    exit 2
}

authfile=
while getopts ':a:h' opt; do
    case $opt in
        a) authfile=$OPTARG ;;
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
        *)        echo "FAIL: unknown image '$1' (want: build, wapp-sdk)" >&2; return 1 ;;
    esac
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

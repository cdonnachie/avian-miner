#!/bin/bash
# Build the portable Linux release in a container, so the binary runs on a
# mining rig rather than only on this machine.
#
#   scripts/linux/build.sh
#
# Run it from WSL or from Linux, wherever docker is. It is deliberately
# separate from scripts/release.sh: that one builds against whatever the host
# has, which is right for a local build and produces a tarball needing
# GLIBC_2.39. Nothing in release.sh changes until this is proven on a rig.
#
# Overridable: SV2 (the avian-sv2 workspace), OUT, IMAGE, AVIAN_CFLAGS,
# CUDA_TAG, RUST_VERSION.
set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MINER=$(cd "$HERE/../.." && pwd)
SV2=${SV2:-$(cd "$MINER/.." && pwd)/avian-sv2}
OUT=${OUT:-$(cd "$MINER/.." && pwd)/avian-miner-release/container}
IMAGE=${IMAGE:-avian-miner-build:22.04}

command -v docker >/dev/null || { echo "no docker on PATH"; exit 1; }

# A release has to be rebuildable from a commit, and the container clones
# rather than copies, so an uncommitted change here would simply not be built
# and the binary would look like the change never happened.
#
# --no-optional-locks because this usually runs from WSL against a checkout on
# an NTFS volume, where git cannot write .git/index.lock: it fails with "Bad
# file descriptor" and then reports a clean tree as dirty. Without the lock it
# cannot refresh the index, which is fine, since nothing here needs to.
# Untracked files are ignored, since the clone would not carry them either.
for r in "$MINER" "$SV2"; do
    [ -d "$r/.git" ] || continue
    dirty=$(git -C "$r" --no-optional-locks status --porcelain --untracked-files=no) || {
        echo "cannot read the git state of $r"; exit 1; }
    if [ -n "$dirty" ]; then
        echo "$r is dirty; the container builds committed work only"
        echo "$dirty"
        exit 1
    fi
done

# Everything this pulls is public, so the build has no use for the client's
# credential store, and a config left behind by Docker Desktop actively breaks
# it: the helper it names, docker-credential-desktop.exe, is not on PATH inside
# WSL and the pull dies with "error getting credentials". An empty config
# directory sidesteps that without touching the real one. Set DOCKER_CONFIG
# yourself if you need a private registry.
if [ -z "${DOCKER_CONFIG:-}" ]; then
    DOCKER_CONFIG=$(mktemp -d)
    export DOCKER_CONFIG
    trap 'rm -rf "$DOCKER_CONFIG"' EXIT
fi

echo "=== image $IMAGE ==="
docker build \
    ${CUDA_TAG:+--build-arg CUDA_TAG="$CUDA_TAG"} \
    ${RUST_VERSION:+--build-arg RUST_VERSION="$RUST_VERSION"} \
    -t "$IMAGE" "$HERE"

mkdir -p "$OUT"
echo
echo "=== build ==="
echo "  miner $MINER"
echo "  sv2   $SV2"
echo "  out   $OUT"
# No --gpus: nothing here runs a kernel, it only compiles them, so this builds
# on a machine with no GPU at all.
docker run --rm \
    -v "$MINER:/src/miner:ro" \
    ${SV2:+-v "$SV2:/src/sv2:ro"} \
    -v "$OUT:/out" \
    ${AVIAN_CFLAGS:+-e AVIAN_CFLAGS="$AVIAN_CFLAGS"}     ${CUDA_ARCH:+-e CUDA_ARCH="$CUDA_ARCH"} \
    "$IMAGE" bash /src/miner/scripts/linux/in-container.sh

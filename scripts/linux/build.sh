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
for r in "$MINER" "$SV2"; do
    [ -d "$r/.git" ] || continue
    if ! git -C "$r" diff --quiet || ! git -C "$r" diff --cached --quiet; then
        echo "$r is dirty; the container builds committed work only"
        git -C "$r" status --short
        exit 1
    fi
done

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
    ${AVIAN_CFLAGS:+-e AVIAN_CFLAGS="$AVIAN_CFLAGS"} \
    "$IMAGE" bash /src/miner/scripts/linux/in-container.sh

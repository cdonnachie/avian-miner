#!/bin/bash
# Check every algorithm's 80-byte kernel, which is the one that runs at stage
# one of the chain.
#
# The two forced orders used until now happened to begin with bmw and skein,
# so those were the only _80 kernels ever exercised. The natural benchmark
# order begins with sha512 and diverges at depth 1.
#
# A uniform order makes stage one that algorithm's _80 kernel, and tracing
# only depth 1 keeps this to about two minutes for all sixteen.
set -u
SRC=${SRC:-$HOME/src/avian-miner}
R=$(getent group render | cut -d: -f3)
V=$(getent group video | cut -d: -f3)
SECS=${SECS:-6}

docker run --rm \
    --device=/dev/kfd --device=/dev/dri --group-add "$R" --group-add "$V" \
    --ipc=host --shm-size 8G -e SECS="$SECS" \
    -v "$SRC":/src -w /src \
    rocm/dev-ubuntu-24.04:latest \
    bash /src/first-stage-probe.sh

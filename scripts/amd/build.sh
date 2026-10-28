#!/bin/bash
# Build the miner for gfx942 inside the ROCm container, on the droplet.
#
# In a container rather than on the host because the host has no compiler and
# no autotools, and installing them would need root again. The container's
# root is free, and now that amdgpu-dkms claims the card the container sees
# the GPU through the same device nodes the host does.
#
# Logs go to the bind-mounted source tree, not /tmp: the container is --rm, so
# a log written inside it disappears with it, which cost one round trip.
#
# Run on the droplet: bash ~/droplet-build.sh
set -eu
SRC=${SRC:-$HOME/src/cpuminer-avian}
RENDER=$(getent group render | cut -d: -f3)
VIDEO=$(getent group video | cut -d: -f3)
mkdir -p "$SRC/buildlogs"

docker run --rm \
    --device=/dev/kfd --device=/dev/dri \
    --group-add "$RENDER" --group-add "$VIDEO" \
    --ipc=host --shm-size 8G \
    -v "$SRC":/src -w /src \
    rocm/dev-ubuntu-24.04:latest \
    bash -c '
set -eu
L=/src/buildlogs
echo "=== build prerequisites ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq automake autoconf libtool pkg-config libcurl4-openssl-dev \
    libssl-dev libgmp-dev zlib1g-dev > /dev/null
echo "  ok"

echo
echo "=== the GPU this will build for ==="
/opt/rocm/llvm/bin/amdgpu-arch

echo
echo "=== autogen ==="
./autogen.sh > $L/autogen.log 2>&1 || { echo FAILED; tail -20 $L/autogen.log; exit 1; }
echo "  ok"

echo
echo "=== configure --with-hip ==="
# build.sh'"'"'s own CFLAGS. Without -march=native the sph_* sources compile
# their other halves and the link fails on CPU symbols unrelated to the GPU.
CFLAGS="-O3 -march=native -Wall" ./configure --with-curl --with-hip \
    > $L/conf.log 2>&1 || { echo FAILED; tail -30 $L/conf.log; exit 1; }
grep -E "hipcc|GPU architectures|installed AMD GPU|jansson" $L/conf.log | sed "s/^/  /"

echo
echo "=== make ==="
# Removed first so a configure-only change still relinks. Nothing the binary
# depends on changes when only link flags move, so make leaves it alone and
# the old binary is silently kept: that cost a round trip over a missing
# -rpath that was correctly in the Makefile all along.
rm -f avian-miner
# The kernel objects too. Automake tracks headers for the C sources through
# .deps, but the .cu.o suffix rule has no such tracking, so editing a header
# under cuda/upstream leaves every stale kernel object in place and make
# reports success having rebuilt nothing. A correct fix to cuda_helper.h was
# measured as having no effect for exactly this reason.
find cuda algo/x16 -name "*.o" -delete 2>/dev/null || true
make -j"$(nproc)" > $L/make.log 2>&1 || {
    echo "BUILD FAILED, see buildlogs/make.log"
    grep -cE "error:" $L/make.log | sed "s/^/  total error lines: /"
    echo "  --- distinct messages ---"
    grep -oE "error: .*" $L/make.log | sort | uniq -c | sort -rn | head -12 | sed "s/^/  /"
    exit 1; }
echo "  BUILD OK $(stat -c%s avian-miner) bytes"

echo
echo "=== the GPU backend is really in there ==="
for sym in avian_cuda_scan_batch avian_cuda_device_count avian_cuda_device_name ; do
    nm avian-miner 2>/dev/null | grep -q " [Tt] $sym" && echo "  ok   $sym" || echo "  MISSING $sym"
done
if strings avian-miner | grep -q "this build has no GPU backend"; then
    echo "  FAIL compiled without the GPU gate"
else
    echo "  ok   compiled with the GPU gate"
fi
'

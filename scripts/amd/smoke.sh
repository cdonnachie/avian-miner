#!/bin/bash
# Build and check the AMD backend on whatever card this machine has, then
# print a short report to paste back.
#
# Run it inside AMD's ROCm container, which saves installing the ROCm
# userspace. From the top of a checkout:
#
#   docker run --rm -it --device=/dev/kfd --device=/dev/dri \
#       --group-add "$(getent group render | cut -d: -f3)" \
#       --group-add "$(getent group video  | cut -d: -f3)" \
#       --ipc=host --shm-size 8G -v "$PWD":/src -w /src \
#       rocm/dev-ubuntu-24.04:latest bash scripts/amd/smoke.sh
#
# If /dev/kfd does not exist, or rocminfo finds no GPU, the kernel driver is
# the problem rather than anything here: see README.md in this directory.
#
# Nothing is mined and nothing is submitted anywhere. --benchmark hashes
# against a fixed header with the target wide open, so the only thing the
# machine does is compute and compare.
set -u
LOG=${LOG:-/tmp/avian-amd-smoke}
mkdir -p "$LOG"

say() { printf '%s\n' "$*"; }
rule() { say "------------------------------------------------------------"; }

rule
say "Avian AMD smoke test"
rule

# ---------- the card ----------
ARCH=$(/opt/rocm/llvm/bin/amdgpu-arch 2>/dev/null | head -1)
if [ -z "${ARCH:-}" ]; then
    say "no GPU visible to ROCm. Stopping."
    say "  /dev/kfd present: $([ -e /dev/kfd ] && echo yes || echo NO)"
    say "  see scripts/amd/README.md, the kernel driver section"
    exit 1
fi
say "gpu            : $ARCH"
rocminfo 2>/dev/null | awk '
    /Marketing Name/ && !m { sub(/^ *Marketing Name: */, ""); m=1; print "name           : " $0 }
    /Compute Unit:/  && !c { sub(/^ *Compute Unit: */, "");  c=1; print "compute units  : " $0 }
    /Wavefront Size:/&& !w { sub(/^ *Wavefront Size: */, ""); w=1; print "wavefront      : " $0 }'
say "hipcc          : $(hipcc --version 2>/dev/null | head -1)"

# ---------- build ----------
rule
export DEBIAN_FRONTEND=noninteractive
if ! command -v automake > /dev/null; then
    say "installing build prerequisites"
    apt-get update -qq > /dev/null 2>&1
    apt-get install -y -qq automake autoconf libtool pkg-config \
        libcurl4-openssl-dev libssl-dev libgmp-dev zlib1g-dev > /dev/null 2>&1
fi

say "building for $ARCH (a few minutes)"
./autogen.sh > "$LOG/autogen.log" 2>&1 || {
    say "FAILED: autogen, see $LOG/autogen.log"; exit 1; }
# build.sh's own CFLAGS. Without -march=native the sph_* sources compile their
# other halves and the link fails on CPU symbols unrelated to the GPU.
CFLAGS="-O3 -march=native -Wall" ./configure --with-curl --with-hip \
    > "$LOG/configure.log" 2>&1 || {
    say "FAILED: configure, see $LOG/configure.log"; tail -20 "$LOG/configure.log"; exit 1; }
# Removed first because the .cu.o rule tracks no header dependencies, so a
# stale object would otherwise survive a source change and make reports
# success having rebuilt nothing.
rm -f avian-miner
find cuda algo/x16 -name "*.o" -delete 2>/dev/null || true
make -j"$(nproc)" > "$LOG/make.log" 2>&1 || {
    say "FAILED: build, distinct errors:"
    grep -oE "error: .*" "$LOG/make.log" | sort -u | head -8 | sed 's/^/  /'
    exit 1; }
say "build          : ok, $(stat -c%s avian-miner) bytes"

# ---------- every algorithm's 80-byte kernel ----------
rule
say "each algorithm's first-stage kernel against the CPU"
SECS=${SECS:-6} bash scripts/amd/first-stage-probe.sh 2>&1 | sed 's/^/  /'
FIRST=$?

# ---------- the whole chain ----------
rule
say "the full sixteen-stage chain, two orders"
SECS=${SECS:-8} ORDERS="1F80675605446D40 542CB1E146A7D915" \
    bash scripts/amd/verify-inner.sh 2>&1 \
    | grep -E "^=== order|DIFFER|NO SAMPLES|comparisons|PASS|NOT A PASS" | sed 's/^/  /'

# ---------- end to end ----------
rule
say "mining against a fixed header for 60s, nothing submitted anywhere"
timeout 60 ./avian-miner -a x16rt --gpu --benchmark > "$LOG/bench.log" 2>&1 || true
say "  invalid shares : $(grep -c 'did not validate' "$LOG/bench.log" || true)"
say "  hashrate       : $(grep -oE 'Total: [0-9.]+ [kMG]H/s' "$LOG/bench.log" | tail -1)"
say "  faults         : $(grep -ciE 'launch failure|memory access fault|CUDA error' "$LOG/bench.log" || true)"
rule
say "Please send back everything from the first line above."
rule

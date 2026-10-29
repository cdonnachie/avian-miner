#!/bin/bash
# Apply the build-system changes, then configure and build the miner with the
# CUDA backend. The link is the real test: everything so far only proved the
# pieces compile on their own.
set -eu
# Self-locating, so a fresh clone builds. This used to point at a session
# scratchpad, which meant the committed script could not build the
# committed tree: the sources it copied only existed on one machine.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
S=$SCRIPT_DIR
MINER=${MINER:-$(cd "$SCRIPT_DIR/.." && pwd)}
SV2=${SV2:-$HOME/avian-gpu/avian-sv2}
# The upstream the driver is generated from, now vendored at
# vendor/suprminer-jdd rather than expected in a sibling directory. It used
# to default to $HOME/avian-gpu/.suprminer-src, which was absent on any other
# machine, and with set -u that aborted the script just before the generate
# step, so make quietly rebuilt the previous driver and two fixes went
# untested. Vendoring also means an upstream that disappears cannot take the
# only copy of the generator's input with it. See the README beside it.
SRC=${SRC:-$MINER/vendor/suprminer-jdd}

# Prefer the newest 12.x present, since 12.8 was the first with Blackwell.
for v in 12.9 12.8 12.6; do
    if [ -x "/usr/local/cuda-$v/bin/nvcc" ]; then CUDA_PREFIX=/usr/local/cuda-$v; break; fi
done
: "${CUDA_PREFIX:?no CUDA toolkit found under /usr/local}"
export PATH="$CUDA_PREFIX/bin:$HOME/.cargo/bin:$PATH"
echo "using $CUDA_PREFIX ($(nvcc --version | tail -1))"

python3 "$S/fix-device.py" "$MINER/cuda/device.cu"
python3 "$S/patch-throughput.py" "$MINER/cuda/device.cu" 2>/dev/null \
    || echo "   (throughput scaling already applied)"
# And undone: measured on a 3090, upstream's floor is both faster and the only
# setting that produces no bad candidates. See revert-throughput.py.
python3 "$S/revert-throughput.py" "$MINER/cuda/device.cu" 2>/dev/null \
    || echo "   (throughput already at the floor)"

# Regenerate the driver from upstream. Easy to forget, and forgetting it means
# make happily recompiles the previous generation and the fixes never land.
echo "== regenerating the CUDA driver =="
python3 "$MINER/scripts/gen-driver.py" "$SRC/x16r/x16r.cu" "$MINER/algo/x16/x16rt-cuda.cu"
# Assert the fixes are actually in the file. Twice now a build has quietly
# used the previous driver, and a silently stale driver is worse than a
# failed build because the test results look real.
# The driver is split in two: a batch function with no miner-side symbols,
# which becomes a DLL on Windows, and a host half that verifies and submits.
# Linux links the same batch function, so one implementation is exercised by
# every test instead of a Windows-only path nothing covers.
echo "== splitting the driver =="
python3 "$S/make-dll-driver.py" "$MINER/algo/x16/x16rt-cuda.cu" \
                                "$MINER/algo/x16/x16rt-cuda-batch.cu" \
    || { echo "the driver split failed"; exit 1; }
grep -q "avian_cuda_scan_batch" "$MINER/algo/x16/x16rt-cuda-batch.cu" \
    || { echo "the batch driver has no entry point"; exit 1; }

for marker in 'throughput < 0x100' 'work->nonces\[0\] = UINT32_MAX' 'first" : "second'; do
    grep -q "$marker" "$MINER/algo/x16/x16rt-cuda.cu" \
        || { echo "generated driver is missing: $marker"; exit 1; }
done
echo "   fixes present in the generated driver"


echo "== patching the build system =="
python3 "$MINER/scripts/patch-build.py" "$MINER"
python3 "$MINER/scripts/fix-nvcc-detect.py" "$MINER/configure.ac"
python3 "$MINER/scripts/add-arch-override.py" "$MINER/configure.ac"
# SIMD-first chains go to the CPU: the 80-byte SIMD kernel reports only
# bad candidates, so the GPU would spend one epoch in sixteen on work
# that is entirely discarded.

# --avian-selftest lives behind HAVE_AVIAN_SV2, and it is how the GPU backend
# gets checked against hashes a real Avian node produced, so the Rust library
# is worth building even though the GPU work does not otherwise need it.
if [ ! -f "$SV2/target/release/libavian_sv2.a" ] && [ -d "$SV2" ]; then
    echo "== building libavian_sv2 (first time, this takes a while) =="
    ( cd "$SV2" && cargo build --release -p avian-sv2 ) > /tmp/cargo.log 2>&1 \
        || { echo "cargo failed:"; tail -25 /tmp/cargo.log; }
fi

cd "$MINER"
# Reconfiguring changes per-file compiler flags, and make only rebuilds what
# it thinks is stale, which leaves objects from two configurations in the tree.
# That shows up as undefined references to things like V512_BSWAP32, which
# look like missing code but are really a half-rebuilt tree. build.sh starts
# with distclean for the same reason.
if [ "${CLEAN:-1}" = 1 ] && [ -f Makefile ]; then
    echo "== distclean =="
    make distclean > /tmp/distclean.log 2>&1 || echo "   (nothing to clean)"
    rm -rf .cudaobj
fi

echo "== autogen =="
./autogen.sh > /tmp/autogen.log 2>&1 || { tail -20 /tmp/autogen.log; exit 1; }

echo "== configure =="
# One architecture while iterating. CUDA_ARCH=all for the release list.
: "${CUDA_ARCH:=86}"
CUDA_ARG="--with-cuda=$CUDA_PREFIX"
if [ "$CUDA_ARCH" != "all" ]; then
    CUDA_ARG="$CUDA_ARG --with-cuda-arch=$CUDA_ARCH"
fi
SV2_ARG=""
if [ -f "$SV2/target/release/libavian_sv2.a" ]; then
    SV2_ARG="--with-avian-sv2=$SV2"
    echo "   (with the Stratum v2 backend)"
else
    echo "   (without Stratum v2: libavian_sv2.a not built yet)"
fi
# --with-curl is required: this configure.ac only sets LIBCURL when asked, its
# auto-detection having been commented out upstream. build.sh and the deploy
# Dockerfile both pass it, and -march=native matches build.sh.
CFLAGS="-O3 -march=native" ./configure --with-curl $CUDA_ARG $SV2_ARG \
    > /tmp/configure.log 2>&1 || { tail -40 /tmp/configure.log; exit 1; }
grep -iE "GPU architectures|nvcc" /tmp/configure.log | head -5

echo "== make =="
if make -j"$(nproc)" > /tmp/make.log 2>&1; then
    echo "BUILD OK"
    ls -la avian-miner
    echo
    echo "== is the GPU backend really in it? =="
    nm avian-miner | grep -cE " [Tt] (scanhash_x16rt_cuda|register_x16rt_cuda_algo)" || true
    nm avian-miner | grep -E " [Tt] (scanhash_x16rt_cuda|register_x16rt_cuda_algo|cuda_detect_devices)"
    echo "-- linked CUDA runtime --"
    ldd avian-miner | grep -i cuda || echo "(statically linked or absent)"

    echo
    echo "===================================================================="
    echo " Does the GPU agree with Avian? --avian-selftest drives scanhash and"
    echo " compares against hashes a real node reported, so running it with"
    echo " --gpu checks the kernels against consensus rather than against a"
    echo " second copy of our own arithmetic."
    echo "===================================================================="
    echo
    # Written straight to files, not piped through tail. A pipe buffers
    # everything until the process exits, so the first attempt spent 29
    # minutes hung with nothing to show for it. And bounded by timeout,
    # because a scanhash that never finds a nonce would otherwise sit there
    # for as long as it is left alone.
    # `|| true` because timeout returns 124 and set -e would end the script
    # there, which is how the first attempt lost all its GPU output.
    echo "--- CPU, as the control ---"
    timeout 120 ./avian-miner --algo x16rt --avian-selftest > /tmp/selftest-cpu.log 2>&1 || true
    tail -4 /tmp/selftest-cpu.log

    echo
    echo "--- GPU: does it at least run to completion now? ---"
    # --avian-selftest cannot judge a GPU. It sets the target to all ones so
    # the first nonce wins, which suits a serial loop; a GPU launches half a
    # million nonces at once and the final kernels pick a winner with
    # atomicExch, so the nonce it reports is whichever thread wrote last. Any
    # nonce satisfies an all-ones target, so it reports an arbitrary one and
    # disagrees with the node's hash for the block's own nonce. What is worth
    # reading here is whether it gets through all six without stalling.
    timeout 180 ./avian-miner --algo x16rt --gpu --avian-selftest > /tmp/selftest-gpu.log 2>&1 || true
    echo "blocks reached: $(grep -c 'block [0-9]' /tmp/selftest-gpu.log)/6"
    grep -E "Intensity|did not validate" /tmp/selftest-gpu.log | head -4

    echo
    echo "--- can this shell reach the pool? ---"
    for h in 127.0.0.1 "$(ip route show default | awk '{print $3; exit}')"; do
        [ -z "$h" ] && continue
        if timeout 3 bash -c "exec 3<>/dev/tcp/$h/3333" 2>/dev/null; then
            echo "  $h:3333 reachable"
            echo "$h" > /tmp/poolhost
        else
            echo "  $h:3333 not reachable"
        fi
    done
else
    echo "BUILD FAILED"
    grep -nE "error|Error" /tmp/make.log | head -30
fi

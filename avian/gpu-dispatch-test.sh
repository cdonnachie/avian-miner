#!/bin/bash
# Does one binary really drive two vendors?
#
# The part of the dual-vendor path that cannot be tested with one card is the
# dispatch: whether two backends' devices become one numbering, and whether a
# batch reaches the backend that owns a device with that backend's own index
# rather than the miner's. This builds cuda/avian-gpu-stub.c as the AMD
# backend, which owns no hardware and reports what it was asked for, and
# asserts on the miner's own output.
#
# A real libavian_cuda.so is used as-is when there is one, so a machine with
# an Nvidia card tests the mixed case, and a machine with none still tests
# the numbering. Everything happens in a temporary directory holding copies
# of the miner, because the loader looks beside the executable first and a
# stub must never be left where a real build would find it.
#
# Usage: avian/gpu-dispatch-test.sh [build-dir]
set -u

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=${1:-$SRC}
[ -x "$B/avian-miner" ] || { echo "no avian-miner in $B; build it first"; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$B/avian-miner" "$WORK/" || exit 2

HAVE_REAL=no
if [ -f "$B/libavian_cuda.so" ]; then
    cp "$B/libavian_cuda.so" "$WORK/"
    HAVE_REAL=yes
fi

echo "=== building the stub as the AMD backend ==="
# -I$SRC for cuda/avian-cuda-api.h, which is all it includes.
gcc -O1 -fPIC -shared -I"$SRC" -o "$WORK/libavian_amd.so" \
    "$SRC/cuda/avian-gpu-stub.c" || { echo "  stub did not build"; exit 2; }
echo "  libavian_amd.so: $(stat -c %s "$WORK/libavian_amd.so") bytes"

echo
echo "=== enumeration ==="
cd "$WORK" || exit 2
# --benchmark rather than a pool, so nothing needs a network, and a short
# timeout because the stub never finds anything and would run for ever.
AVIAN_GPU_STUB_DEVICES=2 timeout 12 ./avian-miner -a x16rt --gpu --benchmark \
    > out.log 2>&1
tr -d '\r' < out.log | grep -E "GPU #|STUB:|mining thread" | head -14

fail=0
check() {
    if tr -d '\r' < out.log | grep -qE "$2"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1"; fail=1
    fi
}

echo
echo "=== assertions ==="
if [ "$HAVE_REAL" = yes ]; then
    # The real card is first, so the stub's two devices are 1 and 2. That
    # offset is the thing being tested: both backends number from zero and
    # only the miner's list is continuous.
    check "the Nvidia card is GPU #0"      'GPU #0: .*\(Nvidia\)'
    check "the stub's first card is GPU #1" 'GPU #1: STUB AMD device 0 \(AMD\)'
    check "the stub's second is GPU #2"     'GPU #2: STUB AMD device 1 \(AMD\)'
    check "three mining threads, one per card" '3 GPU mining threads? started'
    # Global 1 and 2 must arrive at the backend as its own 0 and 1. A batch
    # carrying device=1 for the miner's GPU #1 would be this layer leaking
    # its own numbering into the backend, and on real hardware it would mine
    # the wrong card or none.
    check "GPU #1 reaches the stub as its device 0" 'STUB: batch thr_id=1 device=0'
    check "GPU #2 reaches the stub as its device 1" 'STUB: batch thr_id=2 device=1'
else
    echo "  (no libavian_cuda.so here, so this is the stub alone)"
    check "the stub's first card is GPU #0" 'GPU #0: STUB AMD device 0 \(AMD\)'
    check "the stub's second is GPU #1"     'GPU #1: STUB AMD device 1 \(AMD\)'
    check "two mining threads, one per card" '2 GPU mining threads started'
    check "GPU #0 reaches the stub as its device 0" 'STUB: batch thr_id=0 device=0'
    check "GPU #1 reaches the stub as its device 1" 'STUB: batch thr_id=1 device=1'
fi
check "the diagnostics callbacks were handed over" 'STUB: diagnostics wired \(hash_n set, prehash set\)'

echo
echo "=== a backend that lies about its ABI is refused ==="
# The one failure the version check exists for. Built with the version
# forced to something else, the loader must ignore the library rather than
# call into it, because the job struct is plain data and a mismatch would be
# read with the fields in the wrong places.
AVIAN_GPU_STUB_ABI=99 timeout 12 ./avian-miner -a x16rt --gpu --benchmark \
    > abi.log 2>&1
if tr -d '\r' < abi.log | grep -q "speaks ABI 99, this miner speaks"; then
    echo "  ok   refused, and said why"
else
    echo "  FAIL took it, or said nothing"; fail=1
    tr -d '\r' < abi.log | grep -iE "abi|backend" | head -3
fi
if tr -d '\r' < abi.log | grep -q "STUB:"; then
    echo "  FAIL it was called anyway"; fail=1
else
    echo "  ok   never called"
fi

echo
[ $fail -eq 0 ] && echo "PASS" || echo "FAILED"
exit $fail

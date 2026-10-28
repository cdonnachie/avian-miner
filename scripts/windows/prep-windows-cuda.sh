#!/bin/bash
# Prepare the Windows checkout for a CUDA DLL build, from scratch each time.
#
# Three steps that must happen in this order, which is why they live in one
# script rather than being run by hand: staging overwrites the shim, so the
# two patches have to follow it, and the shim patch is idempotent and would
# otherwise refuse to re-apply after a restage.
#
#   1. stage the CUDA sources from the WSL tree (sources only, no objects)
#   2. point POSIX includes at a small MSVC compat header
#   3. give the shim a DLL path that does not need miner.h
set -u
# The three patch scripts below live beside this one and are tracked, so they
# are located relative to it. They used to be read out of a session scratchpad
# directory, which meant a fresh clone of this repository could not run the
# build at all, and clearing temp broke it on the machine that wrote it.
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# Overridable so a clone somewhere else can be staged without editing this.
W=${W:-/mnt/c/development/cpuminer-avian}
SCRIPTS=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

echo "########## 1. stage ##########"
bash "$S/stage-cuda-windows.sh" || exit 1

echo
echo "########## 2. POSIX compat ##########"
python3 "$S/win-cuda-compat.py" "$W" || exit 1

echo
echo "########## 3. DLL shim ##########"
python3 "$S/dll-shim-patch.py" "$W" || exit 1

echo
echo "########## 4. the AES barriers ##########"
# Re-applied here, not inherited. Staging copies the vendored tree
# from the Linux checkout, and if that copy has drifted the Windows
# DLL is built without them: ECHO and SHAVITE chains then disagree
# at 12 to 26 percent while Linux is clean, which is exactly what
# happened. Idempotent, so it costs nothing when they are present.
python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/fix-aes-barrier.py" "$W/cuda/upstream" || exit 1

echo
echo "########## 5. the skein abort ##########"
# Upstream exits the process from skein512_cpu_setBlock_80 over an
# error it did not cause. Re-applied here because staging overwrites
# the vendored file from the Linux tree.
python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/fix-skein-abort.py" "$W" || exit 1

echo
echo "########## 6. the portable lop3 forms ##########"
# Re-applied here for the same reason as the barriers: staging copies the
# vendored tree from the Linux checkout, and a re-vendor there would drop
# the guarded plain-C forms with no build failing to say so. Idempotent.
# Selected only when AVIAN_NO_PTX is set, so this does not change what the
# default Windows DLL compiles.
python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/make-ptx-portable.py" "$W/cuda/upstream" || exit 1

echo
echo "########## 7. the HIP-visible __CUDA_ARCH__ guards ##########"
# hipcc never defines __CUDA_ARCH__. That is what hands us the guarded
# plain-C fallbacks for free, and the same fact breaks three other things:
# the host declarations of blockDim and threadIdx in both helper headers.
# Harmless to the CUDA build, where the condition is unchanged.
python3 "$SCRIPTS/fix-hip-guards.py" "$W/cuda/upstream" || exit 1

echo
echo "########## 8. the portable xor.b64 forms ##########"
# The xor1/3/5/8/9 helpers are chains of inline PTX for an operation every
# compiler emits optimally on its own. Selected only when AVIAN_NO_PTX is
# set, so this does not change what the default Windows DLL compiles.
python3 "$SCRIPTS/fix-xor-asm.py" "$W/cuda/upstream" || exit 1

echo
echo "########## 9. kernel-launch syntax ##########"
# Upstream writes launches as `f << <grid, block >> > (args)`. nvcc accepts
# the spaces and clang in HIP mode does not, reporting "expected expression"
# about a line that is plainly a launch. The normalised form is identical to
# nvcc, so this is applied unconditionally rather than behind a flag.
python3 "$SCRIPTS/fix-launch-syntax.py" "$W/cuda" || exit 1
python3 "$SCRIPTS/fix-launch-syntax.py" "$W/algo/x16" || exit 1

echo
echo "########## assertions ##########"
fail=0
grep -q "AVIAN_CUDA_BUILDING_DLL" "$W/cuda/avian-cuda-shim.h" \
    && echo "  ok   shim has a DLL path" || { echo "  FAIL shim DLL path"; fail=1; }
grep -q "__declspec(thread)" "$W/cuda/avian-cuda-shim.h" \
    && echo "  ok   __thread mapped for MSVC" || { echo "  FAIL __thread macro"; fail=1; }
grep -q "extern int opt_debug" "$W/cuda/avian-cuda-shim.h" \
    && echo "  ok   opt_debug declared" || { echo "  FAIL opt_debug"; fail=1; }
grep -q "avian-cuda-win-compat" "$W/cuda/device.cu" \
    && echo "  ok   device.cu uses the compat header" || { echo "  FAIL device.cu"; fail=1; }
# Informational. audit-aes.py disagrees with the file in front of me
# on sites that plainly have a barrier, so it is not trustworthy
# enough to fail a build on. fix-aes-barrier.py places them
# deterministically; this just prints what the audit thinks.
python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/audit-aes.py" "$W/cuda/upstream" 2>&1 \n    | grep -vE "ok$" | sed "s/^/  audit: /" || true
grep -q "avian-cuda-win-compat" "$W/algo/x16/x16r-gate.h" \
    && { echo "  FAIL the miner's own header was patched"; fail=1; } \
    || echo "  ok   the miner's headers were left alone"
exit $fail

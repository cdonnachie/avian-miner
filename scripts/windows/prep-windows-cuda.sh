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
S=/mnt/c/Users/craig/AppData/Local/Temp/claude/c--development-Avian-v5-0/9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635/scratchpad
W=/mnt/c/development/cpuminer-avian

echo "########## 1. stage ##########"
bash "$S/stage-cuda-windows.sh" || exit 1

echo
echo "########## 2. POSIX compat ##########"
python3 "$S/win-cuda-compat.py" "$W" || exit 1

echo
echo "########## 3. DLL shim ##########"
python3 "$S/dll-shim-patch.py" "$W" || exit 1

echo
echo "########## 4. the skein abort ##########"
# Upstream exits the process from skein512_cpu_setBlock_80 over an
# error it did not cause. Re-applied here because staging overwrites
# the vendored file from the Linux tree.
python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/fix-skein-abort.py" "$W" || exit 1

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
grep -q "avian-cuda-win-compat" "$W/algo/x16/x16r-gate.h" \
    && { echo "  FAIL the miner's own header was patched"; fail=1; } \
    || echo "  ok   the miner's headers were left alone"
exit $fail

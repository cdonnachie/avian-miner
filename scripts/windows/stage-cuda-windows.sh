#!/bin/bash
# Stage the CUDA sources into the Windows checkout so the DLL can be built.
#
# The two trees are separate: all CUDA work lives in the WSL clone, and the
# Windows checkout has never had a cuda/ directory. Only sources travel, not
# objects, because a Linux .o in a tree MSVC is about to build is exactly the
# kind of thing that links and then misbehaves.
set -u
L=$HOME/avian-gpu/cpuminer-avian
W=/mnt/c/development/cpuminer-avian

[ -d "$L/cuda" ] || { echo "no cuda/ in the WSL tree"; exit 1; }
[ -d "$W" ] || { echo "no Windows checkout at $W"; exit 1; }

echo "=== copying sources (no objects) ==="
rm -rf "$W/cuda"
mkdir -p "$W/cuda"
( cd "$L" && find cuda -type f \
    \( -name '*.cu' -o -name '*.cuh' -o -name '*.h' -o -name '*.hpp' \) -print0 ) \
  | ( cd "$L" && xargs -0 -I{} cp --parents {} "$W/" )

for f in algo/x16/x16rt-cuda-batch.cu algo/x16/x16rt-cuda.h \
         algo/x16/x16rt-cuda-gate.c algo/x16/x16rt-cuda-host.c; do
    if [ -f "$L/$f" ]; then
        cp "$L/$f" "$W/$f"
        echo "  $f"
    else
        echo "  MISSING in the WSL tree: $f"
    fi
done

echo
echo "=== staged ==="
echo "  .cu files:  $(find "$W/cuda" "$W/algo/x16" -name '*.cu' 2>/dev/null | wc -l)"
echo "  .cuh files: $(find "$W/cuda" -name '*.cuh' 2>/dev/null | wc -l)"
echo "  headers:    $(find "$W/cuda" -name '*.h' 2>/dev/null | wc -l)"
echo "  objects:    $(find "$W/cuda" -name '*.o' 2>/dev/null | wc -l)  (must be 0)"
du -sh "$W/cuda" 2>/dev/null | sed 's/^/  size: /'

echo
echo "=== the batch driver is the generated one, not a stale copy ==="
if grep -q 'avian_cuda_scan_batch' "$W/algo/x16/x16rt-cuda-batch.cu" 2>/dev/null; then
    echo "  ok   entry point present"
else
    echo "  FAIL no entry point in the staged batch driver"
    exit 1
fi
if cmp -s "$L/algo/x16/x16rt-cuda-batch.cu" "$W/algo/x16/x16rt-cuda-batch.cu"; then
    echo "  ok   identical to the WSL copy that Linux just mined with"
else
    echo "  FAIL the staged copy differs from the one Linux validated"
    exit 1
fi

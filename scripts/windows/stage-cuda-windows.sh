#!/bin/bash
# Stage the CUDA sources into the Windows checkout so the DLL can be built.
#
# Only needed when the kernels are being changed in the Linux tree and you
# want them on Windows before committing. cuda/** is tracked, so a plain
# build from a clean checkout does not need this at all and the script says
# so and exits.
#
# When it does copy, only sources travel, not objects, because a Linux .o in
# a tree MSVC is about to build is exactly the kind of thing that links and
# then misbehaves.
set -u
# Both overridable. They were hard-coded, which made the Windows DLL build
# require a WSL clone at one particular path.
L=${L:-$HOME/avian-gpu/avian-miner}
# Derived rather than named. This stage runs under WSL, which sees
# the tree as /mnt/c/..., and the derivation reports it in whatever
# notation the running shell uses, so no form is written out.
W=${W:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}

[ -d "$W" ] || { echo "no Windows checkout at $W"; exit 1; }

# Staging is now optional. It mattered when the Windows checkout had no cuda/
# directory of its own, but cuda/** and the four algo/x16 files are tracked,
# so git already delivers what this used to copy. The copy is kept for the
# case it is still good for: iterating on the kernels in the Linux tree and
# wanting them on Windows before committing. Without a Linux tree, the
# tracked files are used as they are.
if [ ! -d "$L/cuda" ]; then
    echo "=== no Linux tree at $L: using the tracked files in $W ==="
    missing=0
    for f in cuda/upstream \
             algo/x16/x16rt-cuda-batch.cu \
             algo/x16/x16rt-cuda.h \
             algo/x16/x16rt-cuda-gate.c \
             algo/x16/x16rt-cuda-host.c; do
        if [ -e "$W/$f" ]; then
            echo "  ok      $f"
        else
            echo "  MISSING $f"
            missing=1
        fi
    done
    [ "$missing" = 0 ] || {
        echo "  the checkout is incomplete; this cannot be built from it"
        exit 1; }
    echo "  .cu files: $(find "$W/cuda" "$W/algo/x16" -name '*.cu' 2>/dev/null | wc -l)"
    exit 0
fi

# Refuse to stage from a Linux tree that is behind the Windows one.
#
# Staging overwrites cuda/** wholesale, so a stale source tree silently
# reverts committed work with no build failing to say so. That has now
# happened four times: build-cuda-dll.ps1 copying a stale support file, a
# commit capturing the tree it had clobbered, a hand edit to a generated
# driver, and this step reverting the hip_runtime.h include in
# avian-ptx-portable.h minutes after it was committed.
#
# Both trees are clones of the same repository, so the question is just
# whether the Windows HEAD is an ancestor of the Linux one.
wh=$(git -C "$W" rev-parse HEAD 2>/dev/null || echo unknown)
lh=$(git -C "$L" rev-parse HEAD 2>/dev/null || echo unknown)
if [ "$wh" != "unknown" ] && [ "$lh" != "unknown" ] && [ "$wh" != "$lh" ]; then
    if git -C "$L" merge-base --is-ancestor "$wh" "$lh" 2>/dev/null; then
        echo "=== Linux tree is ahead, staging its newer cuda/ ==="
    else
        echo "=== REFUSING TO STAGE ==="
        echo "  Windows HEAD $(echo "$wh" | cut -c1-9) is not an ancestor of"
        echo "  Linux   HEAD $(echo "$lh" | cut -c1-9), so this copy would"
        echo "  revert committed work. Pull in $L first, or pass"
        echo "  L=/nonexistent to use the tracked files as they are."
        exit 1
    fi
fi

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

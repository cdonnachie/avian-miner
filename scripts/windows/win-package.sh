#!/bin/bash
# Collect avian-miner.exe and every non-system DLL it needs, transitively.
#
# The first level is short, but MSYS2's libcurl drags in OpenSSL, zstd,
# brotli, nghttp2, idn2 and more. Walking only the direct imports produces a
# zip that works on the build machine and fails everywhere else, which is the
# classic way to ship a broken Windows build.
set -u
# The build directory, a sibling of the repository unless
# AVIAN_BUILD_DIR says otherwise. Computed from this script rather than
# written out, so the PowerShell and MSYS2 stages of the pipeline agree
# on where each other's output went.
BUILD_ROOT=${AVIAN_BUILD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/avian-winbuild}
B=${B:-$BUILD_ROOT/avian-miner}
REL=${REL:-$BUILD_ROOT/release}

rm -rf "$REL"; mkdir -p "$REL"
cp "$B/avian-miner.exe" "$REL/" || exit 1

# Breadth-first over the import tables. Anything present in /mingw64/bin is
# ours to ship; anything else is a Windows system DLL and must not be copied.
declare -A seen
queue=("$REL/avian-miner.exe")
while [ ${#queue[@]} -gt 0 ]; do
    cur="${queue[0]}"; queue=("${queue[@]:1}")
    for d in $(objdump -p "$cur" 2>/dev/null | awk '/DLL Name:/ {print $3}'); do
        key=$(echo "$d" | tr 'A-Z' 'a-z')
        [ -n "${seen[$key]:-}" ] && continue
        seen[$key]=1
        if [ -f "/mingw64/bin/$d" ]; then
            cp -n "/mingw64/bin/$d" "$REL/"
            queue+=("$REL/$d")
        fi
    done
done

# The GPU backend, when one was built. avian_cuda.dll carries the kernels;
# VCRUNTIME140.dll is what nvcc's MSVC host code needs, and is the only
# Microsoft runtime required because nvcc links the CUDA runtime statically.
# The UCRT (api-ms-win-crt-*) is part of Windows 10 and later.
# $B is the build clone; the DLL sits beside it under the same work
# directory. Derived rather than assuming $WORK, which this script
# does not define.
CUDA_DLL=${CUDA_DLL:-$(dirname "$B")/cuda/avian_cuda.dll}
if [ -f "$CUDA_DLL" ]; then
    cp "$CUDA_DLL" "$REL/"
    echo "  + avian_cuda.dll (GPU backend)"
    for vc in /c/Windows/System32/VCRUNTIME140.dll               "/c/Program Files/Microsoft Visual Studio/2022/Professional/VC/Redist/MSVC"/*/x64/Microsoft.VC143.CRT/vcruntime140.dll; do
        if [ -f "$vc" ]; then cp "$vc" "$REL/VCRUNTIME140.dll"; echo "  + VCRUNTIME140.dll"; break; fi
    done
else
    echo "  (no GPU backend: $CUDA_DLL not present)"
fi

echo "=== packaged ==="
ls -1 "$REL" | sed 's/^/  /'
echo
echo "  $(ls -1 "$REL" | wc -l) files, $(du -sh "$REL" | cut -f1)"

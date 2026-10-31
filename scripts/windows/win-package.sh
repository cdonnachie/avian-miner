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

# The OpenCL backend before the walk rather than after it, unlike the other
# two: make builds this one with the same MinGW compiler as the miner, so it
# has MinGW runtime imports of its own that have to be walked as well. The
# vendor backends come out of MSVC and carry their runtime differently, which
# is why they are handled further down.
OCL_DLL=${OCL_DLL:-$B/avian_ocl.dll}
if [ -f "$OCL_DLL" ]; then
    cp "$OCL_DLL" "$REL/"
    echo "  + avian_ocl.dll (OpenCL GPU backend)"
    # The two tests beside it, when the OpenCL stage built them. They are what
    # a tester on an untried card runs first: neither needs the GPU to be
    # idle, and between them they say whether the chain is right before
    # anybody looks at a hashrate.
    for t in avian-ocl-selftest.exe avian-ocl-batchtest.exe; do
        [ -f "$B/$t" ] && cp "$B/$t" "$REL/" && echo "  + $t"
    done
else
    echo "  (no OpenCL backend: $OCL_DLL not present)"
fi

# Breadth-first over the import tables. Anything present in /mingw64/bin is
# ours to ship; anything else is a Windows system DLL and must not be copied.
declare -A seen
queue=("$REL/avian-miner.exe")
for f in avian_ocl.dll avian-ocl-selftest.exe avian-ocl-batchtest.exe; do
    [ -f "$REL/$f" ] && queue+=("$REL/$f")
done
while [ ${#queue[@]} -gt 0 ]; do
    cur="${queue[0]}"; queue=("${queue[@]:1}")
    for d in $(objdump -p "$cur" 2>/dev/null | awk '/DLL Name:/ {print $3}'); do
        key=$(echo "$d" | tr 'A-Z' 'a-z')
        [ -n "${seen[$key]:-}" ] && continue
        seen[$key]=1
        # The one DLL in /mingw64/bin that must not ship. It is the Khronos
        # ICD loader, and the loader that has to be used is the driver's own
        # in System32; the application directory comes first in the search
        # order, so a copy here would shadow it. Shipping none of it is also
        # what makes the test worth running, since the question being asked
        # is whether the backend works on a stock driver.
        [ "$key" = opencl.dll ] && continue
        if [ -f "/mingw64/bin/$d" ]; then
            cp -n "/mingw64/bin/$d" "$REL/"
            queue+=("$REL/$d")
        fi
    done
done

# The GPU backends, when any were built. Both are copied when both exist:
# that is what a machine with an Nvidia and an AMD card needs, and the miner
# takes whichever of them loads. avian_cuda.dll carries the kernels;
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
    echo "  (no Nvidia backend: $CUDA_DLL not present)"
fi

# The AMD backend, independently of the Nvidia one: a rig with one card of
# each needs both files, and the miner loads whichever are there. Its own HIP
# runtime is deliberately not handled here, because whether to ship one
# inverts between the 6.x and 7.x SDKs and getting it the wrong way round is
# silent both times; scripts/windows/package-amd-test.ps1 is what knows the
# rule. See doc/building.md.
AMD_DLL=${AMD_DLL:-$(dirname "$B")/hip/avian_amd.dll}
if [ -f "$AMD_DLL" ]; then
    cp "$AMD_DLL" "$REL/"
    echo "  + avian_amd.dll (AMD GPU backend)"
else
    echo "  (no AMD backend: $AMD_DLL not present)"
fi

echo "=== packaged ==="
ls -1 "$REL" | sed 's/^/  /'
echo
echo "  $(ls -1 "$REL" | wc -l) files, $(du -sh "$REL" | cut -f1)"

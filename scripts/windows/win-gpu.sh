#!/bin/bash
# Link the CUDA DLL into the Windows miner.
#
# Run after win-build.sh and win-sv2.sh, on the same clone: those produce a
# working CPU miner with Stratum v2, and this adds the GPU backend to it.
#
# The miner does not compile a single .cu file. nvcc will only use MSVC as
# its host compiler, while this miner and its Rust Stratum v2 library are
# MinGW, so the kernels live in avian_cuda.dll (scripts/windows/
# build-cuda-dll.ps1) and the miner links its import library. All that is
# needed here is the gate, the host half, -DHAVE_CUDA, and the .lib.
#
# That the list is this short is the point of the split: the seam carries
# plain data, so the miner needs no CUDA headers, no nvcc, and no knowledge
# of the kernels beyond cuda/avian-cuda-api.h.
set -u
# The build directory, a sibling of the repository unless
# AVIAN_BUILD_DIR says otherwise. Computed from this script rather than
# written out, so the PowerShell and MSYS2 stages of the pipeline agree
# on where each other's output went.
BUILD_ROOT=${AVIAN_BUILD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/avian-winbuild}
WORK=${WORK:-$BUILD_ROOT}
B=$WORK/cpuminer-avian
DLLDIR=${DLLDIR:-$WORK/cuda}
SV2_SRC=${SV2_SRC:-/c/development/avian-sv2}

[ -d "$B" ] || { echo "no build clone at $B; run win-build.sh first"; exit 1; }
[ -f "$DLLDIR/avian_cuda.lib" ] || {
    echo "no avian_cuda.lib in $DLLDIR; run build-cuda-dll.ps1 first"; exit 1; }
cd "$B" || exit 1

echo "=== adding the GPU backend to the build ==="
python3 - "$B" "$DLLDIR" <<'PY'
import io, os, sys
b, dll = sys.argv[1], sys.argv[2]
p = os.path.join(b, "Makefile.am")
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
# Tested against avian_cuda.lib, not against x16rt-cuda-host.c, because the
# Linux `if HAVE_CUDA` block in the committed Makefile.am names that source
# too. Matching on it would make this stage believe its own edit was already
# there, skip it, and produce a Windows binary with no GPU backend while
# reporting success. The import library appears only here.
if "avian_cuda.lib" in s:
    print("  Makefile.am already has the Windows GPU backend")
else:
    s = s.rstrip("\n") + """

# The Windows GPU build. No .cu here: the kernels are in avian_cuda.dll,
# built by nvcc with MSVC, and this links its import library. See
# cuda/avian-cuda-api.h for why the seam is where it is.
avian_miner_SOURCES += algo/x16/x16rt-cuda-gate.c \\
                       algo/x16/x16rt-cuda-host.c
avian_miner_LDADD += %s/avian_cuda.lib
""" % dll.replace("\\", "/")
    io.open(p, "w", encoding="utf-8", errors="surrogateescape",
            newline="").write(s)
    print("  Makefile.am: gate, host half and the import library")
PY

echo
echo "=== the option the backend is reached through ==="
# --gpu and opt_gpu are added at build time rather than carried in the
# source, the same way configure-build.sh does it on Linux. Without this the
# binary links the backend and then refuses the only flag that reaches it:
# "unknown option -- gpu", with the banner still saying "on CPU".
SCRIPTS=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# The long-option table lives in miner.h, not cpu-miner.c, which is where
# cpu-miner.c only gets the handler and the flag.
grep -q '{ "gpu", 0, NULL, 1207 }' "$B/miner.h" \
    || { echo "  --gpu is not in miner.h's option table"; exit 1; }
grep -q "case 1207" "$B/cpu-miner.c" \
    || { echo "  --gpu has no handler in cpu-miner.c"; exit 1; }
echo "  --gpu registered"

echo
echo "=== regenerate the build system ==="
# Makefile.am is input to automake, not to make. Without this the edit above
# never reaches Makefile.in, configure regenerates the same Makefile as
# before, make has nothing to do, and the build reports OK while producing a
# byte-identical binary with no GPU backend in it.
./autogen.sh > /tmp/win-gpu-ag.log 2>&1 \
    || { echo "  autogen failed"; tail -15 /tmp/win-gpu-ag.log; exit 1; }
echo "  automake rerun"

echo
echo "=== reconfigure with HAVE_CUDA ==="
# Keeps whatever win-sv2.sh configured, and adds the one define the gate and
# cpu-miner.c test for.
OLD=$(grep -m1 '^  \$ \./configure' config.log 2>/dev/null | sed 's/^  \$ //')
if [ -z "$OLD" ]; then
    echo "  cannot recover the previous configure line from config.log"; exit 1
fi
echo "  previous: $OLD"
eval "$OLD CPPFLAGS=\"\${CPPFLAGS:-} -DHAVE_CUDA\"" > /tmp/win-gpu-cfg.log 2>&1 \
    || { echo "  configure failed"; tail -20 /tmp/win-gpu-cfg.log; exit 1; }
echo "  configured"

echo
echo "=== rebuild everything, not just what changed ==="
# Adding -DHAVE_CUDA changes how cpu-miner.c and x16r-gate.c compile, but
# automake does not make objects depend on CPPFLAGS, so make leaves the ones
# the SV2 stage already built. The result links the DLL and still reports
# "this build has no GPU backend", because the miner's own code was compiled
# without the define while the newly added files were compiled with it.
make clean > /dev/null 2>&1

echo
echo "=== make ==="
if make -j"$(nproc)" > /tmp/win-gpu-make.log 2>&1; then
    echo "  BUILD OK"
    ls -l avian-miner.exe | awk '{print "  " $5 " bytes"}'
else
    echo "  BUILD FAILED"
    grep -nE "error|undefined reference" /tmp/win-gpu-make.log | head -15
    exit 1
fi

echo
echo "=== assert the GPU backend is in the artifact ==="
fail=0
if strings avian-miner.exe 2>/dev/null | grep -q "avian_cuda.dll"; then
    echo "  ok   imports avian_cuda.dll"
else
    echo "  FAIL no import of avian_cuda.dll"; fail=1
fi
if objdump -p avian-miner.exe 2>/dev/null | grep -qi "avian_cuda"; then
    echo "  ok   present in the import table"
else
    echo "  FAIL not in the import table"; fail=1
fi
# Importing the DLL is not the same as being able to reach it. If cpu-miner.c
# was compiled without HAVE_CUDA the binary still links the backend and then
# refuses --gpu, which is exactly what a stale object produced.
if strings avian-miner.exe 2>/dev/null | grep -q "no GPU backend"; then
    echo "  FAIL compiled without HAVE_CUDA: --gpu will be refused"; fail=1
else
    echo "  ok   compiled with HAVE_CUDA"
fi
exit $fail

#!/bin/bash
# Add the GPU backend to the Windows miner.
#
# Run after win-build.sh and win-sv2.sh, on the same clone: those produce a
# working CPU miner with Stratum v2, and this adds the GPU backend to it.
#
# The miner does not compile a single .cu file. nvcc will only use MSVC as
# its host compiler, while this miner and its Rust Stratum v2 library are
# MinGW, so the kernels live in avian_cuda.dll and avian_amd.dll
# (scripts/windows/build-cuda-dll.ps1 and build-hip-dll.ps1). All that is
# needed here is the gate, the host half, the loader and -DHAVE_CUDA.
#
# AVIAN_WITH_OPENCL=1 adds the OpenCL backend, which is the exception to all
# of that. OpenCL compiles its kernels at run time, so avian_ocl.dll is plain
# C with no device compiler behind it, and make builds it here in the same
# pass as the miner. It also needs nothing installed on the machine that runs
# it: the OpenCL.dll in System32 arrives with the AMD and the Nvidia driver,
# which is what makes this the backend a tester can be handed a zip of.
#
# No import library, which is the part that changed: both DLLs export the
# same avian_cuda_* names, so neither can be imported statically without the
# loader binding every call to one of them. The miner calls LoadLibrary on
# each instead, which is what lets one binary drive an Nvidia card, an AMD
# card, or both at once, and means a missing DLL is a backend that is skipped
# rather than a process that will not start.
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
B=$WORK/avian-miner
DLLDIR=${DLLDIR:-$WORK/cuda}
# A sibling of this tree, derived rather than named. $SV2_SRC still
# overrides, which is how a library checked out elsewhere is used.
SV2_SRC=${SV2_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/avian-sv2}

[ -d "$B" ] || { echo "no build clone at $B; run win-build.sh first"; exit 1; }
WITH_OPENCL=${AVIAN_WITH_OPENCL:-0}
if [ "$WITH_OPENCL" = 1 ]; then
    # Headers to compile against and an import library to link. That import
    # library names OpenCL.dll rather than the libOpenCL.dll the same package
    # installs, so what gets built here resolves to the driver's own loader in
    # System32 and nothing has to ship beside it.
    echo "=== OpenCL build dependencies ==="
    pacman -S --needed --noconfirm mingw-w64-x86_64-opencl-headers         mingw-w64-x86_64-opencl-icd 2>&1 | tail -3
# A DLL, not an import library: nothing is linked against any more. Either
# vendor's will do, since the miner takes whichever it finds at run time, and
# the build needs neither to succeed. Checked anyway, because a package built
# with no backend beside it is not one anybody wants to discover later.
elif [ ! -f "$DLLDIR/avian_cuda.dll" ] && [ ! -f "$DLLDIR/avian_amd.dll" ]; then
    echo "no avian_cuda.dll or avian_amd.dll in $DLLDIR;"
    echo "run build-cuda-dll.ps1 or build-hip-dll.ps1 first,"
    echo "or set AVIAN_WITH_OPENCL=1 to build the OpenCL backend here"
    exit 1
fi
cd "$B" || exit 1

echo "=== adding the GPU backend to the build ==="
if [ "$WITH_OPENCL" = 1 ]; then
    # Nothing to add, and adding it is a link error. configure now has a
    # backend of its own to build, so the committed Makefile.am's `if
    # HAVE_CUDA` block already names the gate, the host half and the loader;
    # appending them again puts each object in avian_miner_SOURCES twice and
    # ld says "multiple definition of avian_gpu_init". The append below is
    # for the other case, where configure found no GPU toolkit at all and
    # the backend is a DLL built by a PowerShell stage instead.
    #
    # A clone an earlier non-OpenCL run already appended to has to be undone
    # rather than merely left alone, or that run's stanza is still in the
    # file and the link fails with nothing on screen to say why.
    if grep -q "appended by scripts/windows/win-gpu.sh" "$B/Makefile.am"; then
        sed -i '/^# The Windows GPU build, appended by/,$d' "$B/Makefile.am"
        echo "  removed an earlier run's append, which would double every object"
    else
        echo "  nothing to add: --with-opencl means Makefile.am carries them"
    fi
else
python3 - "$B" "$DLLDIR" <<'PY'
import io, os, sys
b, dll = sys.argv[1], sys.argv[2]
p = os.path.join(b, "Makefile.am")
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
# Tested against this script's own name, not against a source filename,
# because the Linux `if HAVE_CUDA` block in the committed Makefile.am names
# all three of these sources too. Matching on one of those would make this
# stage believe its own edit was already there, skip it, and produce a
# Windows binary with no GPU backend while reporting success. That block used
# to be identified by the import library, which no longer exists.
if "win-gpu.sh" in s:
    print("  Makefile.am already has the Windows GPU backend")
else:
    s = s.rstrip("\n") + """

# The Windows GPU build, appended by scripts/windows/win-gpu.sh. No .cu here:
# the kernels are in avian_cuda.dll and avian_amd.dll, built by nvcc and
# hipcc with MSVC, and nothing links against either. The miner loads whichever
# are present. See cuda/avian-cuda-api.h for why the seam is where it is, and
# cuda/avian-gpu-backend.h for why they are loaded rather than imported.
avian_miner_SOURCES += algo/x16/x16rt-cuda-gate.c \\
                       algo/x16/x16rt-cuda-host.c \\
                       cuda/avian-gpu-backend.c
"""
    io.open(p, "w", encoding="utf-8", errors="surrogateescape",
            newline="").write(s)
    print("  Makefile.am: gate, host half and the backend loader")
PY
fi

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
# --with-opencl makes configure define HAVE_CUDA on its own, so the two are
# not alternatives: that define is what gates the host code either way, and
# passing it here as well keeps this stage reading the same with a vendor DLL
# and without one.
EXTRA=""
[ "$WITH_OPENCL" = 1 ] && EXTRA="--with-opencl"
eval "$OLD $EXTRA CPPFLAGS=\"\${CPPFLAGS:-} -DHAVE_CUDA\"" > /tmp/win-gpu-cfg.log 2>&1 \
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

# The two OpenCL tests, which `make` leaves alone because they are
# check_PROGRAMS. Worth building and shipping: on a card nobody has run this
# on they are the only checks that do not need the GPU to be idle, and the
# selftest names the kernel that failed rather than leaving a wrong hash to
# be found later. Makefile.am sets no TESTS, so this builds them and runs
# nothing.
if [ "$WITH_OPENCL" = 1 ]; then
    echo
    echo "=== make check, which builds the two test programs ==="
    if make check > /tmp/win-gpu-check.log 2>&1; then
        echo "  built avian-ocl-selftest.exe and avian-ocl-batchtest.exe"
    else
        echo "  FAILED to build the test programs"
        grep -nE "error|undefined reference" /tmp/win-gpu-check.log | head -15
        exit 1
    fi
fi

echo
echo "=== assert the GPU backend is in the artifact ==="
fail=0
# The names it will look for at run time, which are string literals now
# rather than import-table entries.
for dll in avian_cuda.dll avian_amd.dll avian_ocl.dll; do
    if strings avian-miner.exe 2>/dev/null | grep -q "$dll"; then
        echo "  ok   knows to look for $dll"
    else
        echo "  FAIL does not know about $dll"; fail=1
    fi
done
# The inverse of what this used to assert, and the point of the change: an
# import would bind every avian_cuda_* call to one DLL and refuse to start
# without it, which is precisely what cannot work when two libraries export
# the same names. Loading them leaves the import table empty.
if objdump -p avian-miner.exe 2>/dev/null | grep -qi "DLL Name: avian_"; then
    echo "  FAIL still statically imports a backend, so only one can load"
    objdump -p avian-miner.exe | grep -i "DLL Name: avian_"
    fail=1
else
    echo "  ok   imports neither, so it can load either or both"
fi
# Carrying the loader is not the same as being able to reach it. If
# cpu-miner.c was compiled without HAVE_CUDA the binary still has the backend
# and then refuses --gpu, which is exactly what a stale object produced.
#
# Anchored on the one literal that only the broken build contains: the #else
# arm of the --gpu option compiles in "this build has no GPU backend", and
# nothing else in the tree says it. Its absence is evidence rather than the
# mere absence of a symptom, which is why there is no second, positive test.
#
# There was one, matching the banner, and it lasted hours: the banner was
# reworded and the assertion failed a correct build. Twice now a check here
# has been broken by a string moving under it, so this one is tied to a
# literal that cannot change without the behaviour it tests changing too.
if strings avian-miner.exe 2>/dev/null | grep -q "this build has no GPU backend"; then
    echo "  FAIL compiled without HAVE_CUDA: --gpu will be refused"; fail=1
else
    echo "  ok   compiled with HAVE_CUDA"
fi

# The one backend this stage builds rather than collects, so it is the one
# that can come out missing. A configure that quietly decided it could not
# find OpenCL still produces a working CPU miner with a GPU loader in it and
# nothing for the loader to load, which reads as "no OpenCL device" on the
# tester's machine and sends them looking at their drivers.
if [ "$WITH_OPENCL" = 1 ]; then
    if [ -f avian_ocl.dll ]; then
        echo "  ok   built avian_ocl.dll ($(stat -c %s avian_ocl.dll) bytes)"
    else
        echo "  FAIL --with-opencl was asked for and no avian_ocl.dll was built"
        fail=1
    fi
fi
exit $fail

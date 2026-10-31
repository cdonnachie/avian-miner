#!/bin/sh
# Compile the kernels with AMD's own compiler, for cards nobody here has.
#
#   cuda/ocl/amd-compile-check.sh [GENERATED_HEADER] [TARGET...]
#
# No AMD card has run this backend. That is a real gap and this does not close
# it, but it closes the half that does not need hardware: whether the kernels
# compile under the compiler an AMD card would use. ROCm ships the same clang
# the OpenCL runtime drives, and it will compile for any gfx target whether or
# not one is plugged in, so every failure mode that is a compiler's opinion
# rather than a card's behaviour is reachable from here.
#
# The program is assembled exactly as avian-ocl.c assembles it, by
# concatenating the embedded sources in table order, because that is what
# makes the interesting failures reachable: thirty-three sources are one
# translation unit, so a name defined twice is a program-wide hazard that no
# single kernel compiled alone would show.
#
# -Werror is deliberate. The failure this catches is a macro redefined with a
# different body, which is a warning rather than an error and changes what the
# text after it means.
#
# What this proves: the kernels compile, for those targets, today. What it
# does not prove: that they hash correctly or at a useful rate on any of them.
# Only avian-ocl-selftest and avian-ocl-batchtest on real hardware say that.
#
# Exits 0 and says so when ROCm is not installed, so it can be run anywhere.
set -eu

hdr=${1:-cuda/ocl/avian-ocl-kernels.h}
if [ $# -gt 0 ]; then shift; fi

# Polaris through RDNA 4: the generations a miner is plausibly holding, plus
# gfx942, which is CDNA3 and not something anyone mines on, but is the one AMD
# card this project can actually rent by the hour and so the one these kernels
# get run on.
targets=${*:-"gfx803 gfx900 gfx906 gfx1010 gfx1030 gfx1100 gfx1201 gfx942"}

rocm=${ROCM_PATH:-/opt/rocm}
clang=$rocm/llvm/bin/clang

if [ ! -x "$clang" ]; then
    echo "no ROCm compiler at $clang, so nothing was checked"
    echo "(set ROCM_PATH, or install rocm-llvm)"
    exit 0
fi
if [ ! -f "$hdr" ]; then
    echo "$hdr: not generated yet; build libavian_ocl.so first" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/dump.c" <<'EOF'
#include <stddef.h>
#include <stdio.h>
#include "avian-ocl-kernels.h"

int main( void )
{
    for ( size_t i = 0; i < AVIAN_OCL_KERNEL_COUNT; i++ )
        fputs( avian_ocl_kernels[i].source, stdout );
    return 0;
}
EOF

${CC:-cc} -O0 -I"$(dirname "$hdr")" -o "$tmp/dump" "$tmp/dump.c"
"$tmp/dump" > "$tmp/combined.cl"
echo "$(wc -c < "$tmp/combined.cl") bytes of OpenCL C, one translation unit"

# __OPENCL_VERSION__ is the runtime's to define, not the compiler's, and
# clang defines only __OPENCL_C_VERSION__. sph-common.cl keys its 64-bit
# typedef on it: without it sph_u64 falls back to the host's long long, which
# clang makes 128 bits in OpenCL mode, and every endian decode fails to match.
# A real driver passes it, so passing it here is fidelity, not a workaround.
rc=0
for t in $targets; do
    if "$clang" -x cl -cl-std=CL1.2 -D__OPENCL_VERSION__=120 \
            -Xclang -finclude-default-header \
            --target=amdgcn-amd-amdhsa -mcpu="$t" \
            -nogpulib -Werror -O2 -c -o "$tmp/$t.o" "$tmp/combined.cl" \
            2> "$tmp/$t.log"
    then
        printf '  %-10s ok\n' "$t"
    else
        printf '  %-10s FAILED\n' "$t"
        sed -n '1,30p' "$tmp/$t.log"
        rc=1
    fi
done

# -nogpulib leaves get_global_id and friends as external calls. This is a
# check on the kernels, not a link of something a card could run.
exit $rc

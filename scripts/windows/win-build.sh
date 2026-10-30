#!/bin/bash
# Build the Avian miner for Windows, natively, in MSYS2's MINGW64 environment.
#
# Native rather than cross-compiled because MSYS2 packages every dependency
# the miner needs, which is the part that makes cross-compiling from Linux
# tedious: upstream's winbuild-cross.sh wants hand-built curl and gmp under
# $HOME/usr/lib.
#
# Built from a fresh clone, not the working checkout, because that checkout
# holds object files from Linux builds and make would happily mix them.
set -u
# The build directory, a sibling of the repository unless
# AVIAN_BUILD_DIR says otherwise. Computed from this script rather than
# written out, so the PowerShell and MSYS2 stages of the pipeline agree
# on where each other's output went.
BUILD_ROOT=${AVIAN_BUILD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/avian-winbuild}
# Derived rather than named, so a clone anywhere works. $SRC still
# overrides, which is how a build from a different tree is done.
SRC=${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
WORK=${WORK:-$BUILD_ROOT}
B=$WORK/avian-miner

echo "=== environment ==="
echo "MSYSTEM=${MSYSTEM:-unset}"
command -v gcc && gcc --version | head -1

echo
echo "=== dependencies ==="
pacman -S --needed --noconfirm mingw-w64-x86_64-jansson 2>&1 | tail -3

echo
echo "=== fresh clone ==="
mkdir -p "$WORK"
rm -rf "$B"
git clone -q "$SRC" "$B" || { echo "clone failed"; exit 1; }
cd "$B" || exit 1
echo "at $(git rev-parse --short HEAD)"

# A clone carries committed history, not the working tree, so uncommitted work
# in the source checkout would silently not be built and the binary would look
# like the change never happened. Carry the modifications across explicitly.
if git -C "$SRC" diff --quiet; then
    echo "  source checkout is clean"
else
    git -C "$SRC" diff > /tmp/worktree.patch
    if git apply /tmp/worktree.patch 2>/tmp/apply.log; then
        echo "  applied $(git -C "$SRC" diff --name-only | wc -l) uncommitted file(s) from the checkout"
    else
        echo "  FAILED to apply the working-tree changes:"; head -5 /tmp/apply.log; exit 1
    fi
fi

# There was a "portability patches" step here calling win-portability.py.
# That file was never committed: it is referenced from 003834a, the first
# commit of this build chain, and has never existed in a clone, so the step
# has printed "No such file or directory" and carried on in every Windows
# build anyone has ever run. The binaries are correct without it, so the
# call is gone rather than restored. Nothing is known to be missing; if
# something turns out to be, it belongs in the tree, not in a script that
# fails quietly.
# --sv2-hashrate is not carried in the source; configure-build.sh adds
# it on Linux and this does the same here. Without it the miner cannot
# tell the pool its real rate, the pool sizes its share limiter from a
# hardcoded 1 kH/s, and every share past the floor comes back
# too-many-shares. The earlier CPU release shipped without it.

echo
echo "=== autogen ==="
./autogen.sh > /tmp/win-autogen.log 2>&1 || { tail -20 /tmp/win-autogen.log; exit 1; }

echo "=== configure (no Stratum v2 yet, to prove the toolchain first) ==="
# --with-curl: this configure.ac only sets LIBCURL when asked.
# --with-curl=/mingw64 rather than bare --with-curl: the bare form leaves
# $with_curl as the literal "yes" and the link line gets -Lyes/lib.
# The baseline runs on anything since about 2009 and uses none of AES,
# AVX2, SHA or VAES. Overridable, but one tier is the measured answer
# rather than a default: against -O3 -maes -mavx2 -msha -mvaes on a 9900X,
# minotaurx gained about 2%, inside the run-to-run noise, while x16rt on
# the CPU gained 2.1x. Minotaurx is yespower and bound by memory rather
# than by vector width, and x16rt on a CPU is not worth mining. Upstream
# ships eight binaries for a hundred algorithms; Avian has one CPU
# algorithm and it is indifferent.
AVIAN_CFLAGS=${AVIAN_CFLAGS:-"-O3 -march=x86-64-v2"}
echo "  CFLAGS: $AVIAN_CFLAGS"
CFLAGS="$AVIAN_CFLAGS" ./configure --with-curl=/mingw64 \
    > /tmp/win-configure.log 2>&1 || { tail -30 /tmp/win-configure.log; exit 1; }
grep -iE "^checking (host|build)|windows" /tmp/win-configure.log | head -4

echo
echo "=== make ==="
if make -j"$(nproc)" > /tmp/win-make.log 2>&1; then
    echo "BUILD OK"
    ls -la avian-miner.exe
else
    echo "BUILD FAILED"
    grep -nE "error|Error [0-9]" /tmp/win-make.log | head -25
    exit 1
fi

echo
echo "=== does it run? ==="
./avian-miner.exe --version 2>&1 | head -4
echo "-- algorithms --"
./avian-miner.exe --help 2>&1 | grep -iE "x16rt|minotaurx" | head -4

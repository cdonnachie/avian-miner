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
SRC=${SRC:-/c/development/cpuminer-avian}
WORK=${WORK:-/c/Users/craig/AppData/Local/Temp/claude/c--development-Avian-v5-0/9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635/scratchpad/winbuild}
B=$WORK/cpuminer-avian

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

echo
echo "=== portability patches ==="
python3 "$(dirname "$0")/win-portability.py" "$B"
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
CFLAGS="-O3 -march=x86-64-v2" ./configure --with-curl=/mingw64 \
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

#!/bin/bash
# Add the Stratum v2 backend to the Windows build.
#
# Without it the binary still lists the --sv2 options in --help, because that
# text is unconditional while the option handlers are guarded, so it would
# advertise a feature it then rejects. Since the pool only speaks Stratum v2,
# a Windows miner without it is of no use.
set -u
SV2_SRC=${SV2_SRC:-/c/development/avian-sv2}
WORK=${WORK:-/c/Users/craig/AppData/Local/Temp/claude/c--development-Avian-v5-0/9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635/scratchpad/winbuild}
B=$WORK/cpuminer-avian
PREFIX=$WORK/sv2-prefix

echo "=== rust ==="
if ! command -v cargo > /dev/null 2>&1; then
    pacman -S --needed --noconfirm mingw-w64-x86_64-rust 2>&1 | tail -2
fi
cargo --version || { echo "no cargo"; exit 1; }
rustc --version
echo "default target: $(rustc -vV | awk -F': ' '/^host/ {print $2}')"

echo
echo "=== building libavian_sv2 for Windows ==="
# Target dir kept out of the checkout: a cargo target/ there is gigabytes and
# the Docker build contexts walk that directory.
export CARGO_TARGET_DIR=$WORK/cargo-target
( cd "$SV2_SRC" && cargo build --release -p avian-sv2 ) > /tmp/win-cargo.log 2>&1 \
    || { echo "cargo failed:"; tail -25 /tmp/win-cargo.log; exit 1; }

LIB=$(find "$CARGO_TARGET_DIR" -name 'libavian_sv2.a' | head -1)
[ -n "$LIB" ] || { echo "no libavian_sv2.a produced"; ls -R "$CARGO_TARGET_DIR/release" | head -20; exit 1; }
echo "  $LIB  ($(du -h "$LIB" | cut -f1))"

# configure wants PREFIX/target/release/libavian_sv2.a and
# PREFIX/crates/avian-sv2/include, which is the layout the deploy Dockerfile
# fakes up too.
rm -rf "$PREFIX"
mkdir -p "$PREFIX/target/release" "$PREFIX/crates/avian-sv2"
cp "$LIB" "$PREFIX/target/release/"
cp -r "$SV2_SRC/crates/avian-sv2/include" "$PREFIX/crates/avian-sv2/"

echo
echo "=== what Windows libraries does the Rust static library want? ==="
# Rust's std on windows-gnu needs these; the C link line has no idea.
for s in bcrypt userenv ntdll advapi32 secur32 crypt32; do
    n=$(nm -u "$LIB" 2>/dev/null | grep -ciE "$s" || true)
    [ "$n" != 0 ] && printf '  %-12s referenced\n' "$s"
done

echo
echo "=== reconfigure with Stratum v2 ==="
cd "$B" || exit 1
make clean > /dev/null 2>&1
CFLAGS="-O3 -march=x86-64-v2" ./configure --with-curl=/mingw64 \
    --with-avian-sv2="$PREFIX" > /tmp/win-configure2.log 2>&1 \
    || { tail -25 /tmp/win-configure2.log; exit 1; }
echo "  configured"

echo
echo "=== make ==="
if make -j"$(nproc)" > /tmp/win-make2.log 2>&1; then
    echo "BUILD OK"
    ls -la avian-miner.exe
else
    echo "BUILD FAILED"
    grep -oE "undefined reference to .[^']+'" /tmp/win-make2.log \
      | sed "s/undefined reference to .//; s/'$//" | sort -u | head -25
    grep -E "cannot find|collect2" /tmp/win-make2.log | sort -u | head -5
    exit 1
fi

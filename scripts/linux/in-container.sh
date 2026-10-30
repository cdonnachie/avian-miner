#!/bin/bash
# The Linux build, run inside the image built from the Dockerfile beside this.
# Not meant to be called directly: scripts/linux/build.sh starts the container
# and mounts what this expects.
#
#   /src/miner   this repository, read-only
#   /src/sv2     the avian-sv2 workspace, read-only
#   /out         where the tarball lands
#
# The mounts are read-only on purpose, so a container can never write into a
# Windows checkout, and everything is cloned into /build before it is touched.
# A clone also means only committed work is built, which is what makes the
# tarball traceable to a commit.
set -eu

MINER_SRC=${MINER_SRC:-/src/miner}
SV2_SRC=${SV2_SRC:-/src/sv2}
OUT=${OUT:-/out}
CUDA=${CUDA:-/usr/local/cuda}
# The same tier the Windows and WSL releases use, and for the same measured
# reason. See the comment in scripts/windows/win-build.sh.
CF=${AVIAN_CFLAGS:-"-O3 -march=x86-64-v2"}
# Empty means every architecture the toolkit can emit, which is what a
# release wants and about an hour. Set it to one, say 86, when the thing
# being tested is the host side and the kernels are beside the point.
ARCH=${CUDA_ARCH:-}

# git refuses to read a repository owned by another uid, which every mount
# from the host is.
git config --global --add safe.directory '*'

AVIAN_VERSION=$(sed -n 's/^AC_INIT(\[[^]]*\], *\[\([^]]*\)\]).*/\1/p' "$MINER_SRC/configure.ac")
[ -n "$AVIAN_VERSION" ] || { echo "cannot read the version from configure.ac"; exit 1; }
PKG=avian-miner-$AVIAN_VERSION

echo "=== host ==="
( . /etc/os-release && echo "$PRETTY_NAME" )
ldd --version | head -1
gcc --version | head -1
"$CUDA/bin/nvcc" --version | tail -2 | head -1
cargo --version

echo
echo "=== clone ==="
rm -rf /build/miner /build/avian-sv2
git clone -q "$MINER_SRC" /build/miner
echo "  miner   $(git -C /build/miner rev-parse --short HEAD)"
SV2=""
if [ -d "$SV2_SRC/.git" ]; then
    git clone -q "$SV2_SRC" /build/avian-sv2
    echo "  sv2     $(git -C /build/avian-sv2 rev-parse --short HEAD)"
    SV2=/build/avian-sv2
else
    echo "  sv2     not mounted, so this will be a Stratum v1 only build"
fi

if [ -n "$SV2" ]; then
    echo
    echo "=== libavian-sv2 ==="
    # In the clone's own target directory, because that is where configure
    # looks: --with-avian-sv2=PATH reads PATH/target/release/libavian_sv2.a.
    #
    # Logged rather than piped to tail: a pipeline's status is the last
    # command's, so `cargo build | tail` returns tail's success and set -e
    # carries straight on past a failed build.
    ( cd "$SV2" && cargo build --release -p avian-sv2 ) > /build/cargo.log 2>&1 || {
        tail -20 /build/cargo.log; exit 1; }
    ls -l "$SV2/target/release/libavian_sv2.a"
fi

echo
echo "=== configure ==="
cd /build/miner
./autogen.sh > /build/autogen.log 2>&1 || { tail -20 /build/autogen.log; exit 1; }
# The committed configure is deliberately stale and has never known
# --with-cuda, so a failed autogen leaves one that warns about "unrecognized
# options" and quietly builds a CPU-only release. Checked rather than hoped.
for opt in with-cuda enable-cudart-static with-avian-sv2; do
    grep -q -- "$opt" configure || {
        echo "configure does not know --$opt, so autogen left a stale one"
        exit 1; }
done
echo "  CFLAGS: $CF"
CFLAGS="$CF" ./configure \
    --with-curl=/usr \
    --with-cuda="$CUDA" \
    ${ARCH:+--with-cuda-arch="$ARCH"} \
    --enable-cudart-static \
    ${SV2:+--with-avian-sv2="$SV2"} \
    > /build/configure.log 2>&1 || { tail -30 /build/configure.log; exit 1; }
grep -E "^checking how to link the CUDA runtime" /build/configure.log || true
# configure warns rather than fails on an option it does not know, so ask
# config.log what it actually took.
took() { grep -q "^$1='.\+'" config.log; }
took NVCC || { echo "CUDA was asked for, configure did not take it"; exit 1; }
[ -z "$SV2" ] || took AVIAN_SV2_LIBS || { echo "Stratum v2 was asked for, configure did not take it"; exit 1; }

echo
echo "=== make ==="
make -j"$(nproc)" > /build/make.log 2>&1 || {
    echo "--- make failed; the summary lines ---"
    grep -nE "error|Error [0-9]" /build/make.log | head -25
    echo "--- and the end of the log, where a link failure says why ---"
    tail -60 /build/make.log
    exit 1; }
ls -l avian-miner libavian_cuda.so

echo
echo "=== what the rig will need ==="
# The whole point of the image, so it is reported rather than assumed. The
# highest GLIBC_* version referenced is the floor: a machine with anything
# older cannot run this at all.
for f in avian-miner libavian_cuda.so; do
    echo "-- $f"
    echo -n "   glibc floor: "
    objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
    echo "   needs:"
    objdump -p "$f" | awk '/NEEDED/ {print "     " $2}'
done

echo
echo "=== package ==="
D=$OUT/$PKG-linux-x86_64
rm -rf "$D"; mkdir -p "$D"
cp avian-miner "$D/"
for so in libavian_cuda.so libavian_amd.so; do [ -f "$so" ] && cp "$so" "$D/"; done
cp README.md LICENSE COPYING doc/licensing.md "$D/"
tar -C "$OUT" -czf "$OUT/$PKG-linux-x86_64.tar.gz" "$PKG-linux-x86_64"
rm -rf "$D"
# The host runs docker as root, so without this the tarball is root-owned on a
# directory the user has to clean up by hand.
chown "$(stat -c %u:%g "$OUT")" "$OUT/$PKG-linux-x86_64.tar.gz" 2>/dev/null || true
ls -l "$OUT/$PKG-linux-x86_64.tar.gz"

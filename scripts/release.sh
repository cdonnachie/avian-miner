#!/bin/bash
# Build, sign and assemble a release.
#
# Three stages, because the work spans three environments that cannot reach
# each other's toolchains. Linux builds under WSL or on a Linux box, Windows
# builds under MSYS2, and signing runs under PowerShell because the
# certificate is a SimplySign cloud card that only CryptoAPI can see. That
# last one is also why none of this belongs in CI: a hosted runner cannot
# hold the key, so the signed artifacts are made here or not at all.
#
#   scripts/release.sh linux     # in WSL, or on Linux
#   scripts/release.sh windows   # in MSYS2 MINGW64
#   scripts/release.sh finish    # either, once both are present
#
# Output goes outside every build tree on purpose. win-package.sh opens with
# rm -rf on its release directory, so a signed artifact left in a build tree
# is one rebuild away from being replaced, unsigned, under whoever is running
# it.
set -eu

STAGE=${1:-}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
VERSION=$(sed -n 's/^AC_INIT(\[[^]]*\], *\[\([^]]*\)\]).*/\1/p' "$REPO/configure.ac")
OUT=${OUT:-$(cd "$REPO/.." && pwd)/avian-miner-release/$VERSION}
NAME=avian-miner-$VERSION

case $STAGE in linux|windows|finish) ;; *)
    echo "usage: $0 linux|windows|finish"; exit 1 ;;
esac
[ -n "$VERSION" ] || { echo "cannot read the version from configure.ac"; exit 1; }

# A release has to be rebuildable from a commit. An uncommitted change here
# would ship in the binary and exist nowhere else.
if ! git -C "$REPO" diff --quiet || ! git -C "$REPO" diff --cached --quiet; then
    echo "the working tree is dirty; commit or stash before releasing"
    git -C "$REPO" status --short
    exit 1
fi
COMMIT=$(git -C "$REPO" rev-parse --short HEAD)
if ! git -C "$REPO" describe --exact-match --tags HEAD >/dev/null 2>&1; then
    echo "note: HEAD ($COMMIT) carries no tag; v$VERSION would normally be one"
fi
mkdir -p "$OUT"
echo "avian-miner $VERSION from $COMMIT -> $OUT"

if [ "$STAGE" = linux ]; then
    B=${B:-$(cd "$REPO/.." && pwd)/avian-linuxbuild}
    rm -rf "$B"; git clone -q "$REPO" "$B"; cd "$B"
    # Not -march=native, which build.sh uses and which is right for a local
    # build and wrong for every machine that is not this one.
    CF=${AVIAN_CFLAGS:-"-O3 -march=x86-64-v2"}
    CUDA=""
    for v in 12.9 12.8 12.6; do
        [ -x "/usr/local/cuda-$v/bin/nvcc" ] && { CUDA="--with-cuda=/usr/local/cuda-$v"; break; }
    done
    [ -x /usr/local/cuda/bin/nvcc ] && [ -z "$CUDA" ] && CUDA="--with-cuda=/usr/local/cuda"
    [ -n "$CUDA" ] || echo "note: no CUDA toolkit found, so this will be a CPU-only release"
    SV2=""
    [ -d "$REPO/../avian-sv2" ] && SV2="--with-avian-sv2=$(cd "$REPO/../avian-sv2" && pwd)"
    ./autogen.sh >/dev/null
    CFLAGS="$CF" ./configure --with-curl $CUDA $SV2 >/dev/null
    make -j"$(nproc)" >/dev/null
    D=$OUT/$NAME-linux-x86_64
    rm -rf "$D"; mkdir -p "$D"
    cp avian-miner "$D/"
    for so in libavian_cuda.so libavian_amd.so; do [ -f "$so" ] && cp "$so" "$D/"; done
    cp "$REPO/README.md" "$REPO/LICENSE" "$REPO/COPYING" "$D/"
    tar -C "$OUT" -czf "$OUT/$NAME-linux-x86_64.tar.gz" "$NAME-linux-x86_64"
    rm -rf "$D"
    echo "  $OUT/$NAME-linux-x86_64.tar.gz"

elif [ "$STAGE" = windows ]; then
    "$REPO/scripts/windows/win-build.sh"
    "$REPO/scripts/windows/win-sv2.sh"
    "$REPO/scripts/windows/win-gpu.sh"
    "$REPO/scripts/windows/win-package.sh"
    REL=${REL:-$(cd "$REPO/.." && pwd)/avian-winbuild/release}
    D=$OUT/$NAME-windows-x64
    rm -rf "$D"; mkdir -p "$D"
    cp "$REL"/* "$D/"
    cp "$REPO/README.md" "$REPO/LICENSE" "$REPO/COPYING" "$D/"
    # Signed in the output directory, after the copy, so nothing rewrites a
    # binary once it carries a signature.
    powershell.exe -ExecutionPolicy Bypass -File \
        "$(cygpath -w "$REPO/scripts/windows/win-sign.ps1")" -Dir "$(cygpath -w "$D")"
    ( cd "$OUT" && zip -qr "$NAME-windows-x64.zip" "$NAME-windows-x64" )
    rm -rf "$D"
    echo "  $OUT/$NAME-windows-x64.zip"

else
    cd "$OUT"
    ls -1 "$NAME"-*.tar.gz "$NAME"-*.zip >/dev/null 2>&1 || {
        echo "nothing to finish: run the linux and windows stages first"; exit 1; }
    sha256sum "$NAME"-*.tar.gz "$NAME"-*.zip > SHA256SUMS
    # Detached and armoured. Authenticode covers the Windows binaries and
    # nothing covers the tarball, so this is what makes the Linux half
    # verifiable at all.
    rm -f SHA256SUMS.asc
    gpg --armor --detach-sign SHA256SUMS
    echo
    cat SHA256SUMS
    echo
    echo "to publish:"
    echo "  gh release create v$VERSION --title 'avian-miner $VERSION' \\"
    echo "    $(ls -1 "$NAME"-*.tar.gz "$NAME"-*.zip SHA256SUMS SHA256SUMS.asc | tr '\n' ' ')"
fi

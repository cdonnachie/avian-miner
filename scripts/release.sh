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
#
# The build directories are per stage and named differently: $B for linux,
# $AVIAN_BUILD_DIR for windows, since that is what the win-* scripts already
# read. On Windows there is no speed reason to move it, both paths being the
# same NTFS volume, but there is a good one: keeping the build tree away from
# wherever a miner is running, because win-package.sh starts with rm -rf.
#   scripts/release.sh finish    # in MSYS2, once both are present
#
# finish is not "either": the signing key is in Gpg4win, and neither MSYS2's
# own gpg nor WSL's can see that keyring. It uses whatever git is configured
# to sign commits with, which is the one gpg that has the key.
#
# The three stages share an output directory because $OUT derives from the
# repository, and /c/development/... under MSYS2 and /mnt/c/development/...
# under WSL are the same folder.
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
    # The AMD backend, when there is a ROCm install to build it with. An
    # explicit target list rather than configure's default, because that asks
    # the machine which AMD GPUs it has and a build host need not have one.
    # The list is cuda/hip/gfx-targets, which build-hip-dll.ps1 reads too, so
    # libavian_amd.so and avian_amd.dll cover the same hardware.
    HIP=""
    HIPCC=""
    for p in /opt/rocm /opt/rocm-* ; do
        [ -x "$p/bin/hipcc" ] && { HIPCC="$p"; break; }
    done
    [ -z "$HIPCC" ] && command -v hipcc > /dev/null 2>&1 && HIPCC=/opt/rocm
    if [ -n "$HIPCC" ] && [ -f "$REPO/cuda/hip/gfx-targets" ]; then
        ARCH=$(grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$REPO/cuda/hip/gfx-targets" | paste -sd, -)
        HIP="--with-hip=$HIPCC --with-hip-arch=$ARCH"
        echo "note: building the AMD backend for $(echo "$ARCH" | tr , " " | wc -w) targets, which is slow"
    else
        echo "note: no ROCm here, so this release carries no AMD backend"
    fi
    SV2=""
    [ -d "$REPO/../avian-sv2" ] && SV2="--with-avian-sv2=$(cd "$REPO/../avian-sv2" && pwd)"
    # Checked, because the committed configure is deliberately stale and has
    # never known --with-cuda: a failed autogen leaves it in place, and
    # configure then warns about "unrecognized options" and builds a release
    # with no GPU backend in it. One run of this stage did exactly that.
    ./autogen.sh >/dev/null || { echo "autogen.sh failed"; exit 1; }
    for opt in with-cuda with-hip with-avian-sv2; do
        grep -q -- "$opt" configure || {
            echo "configure does not know --$opt, so autogen left a stale one"
            exit 1; }
    done
    CFLAGS="$CF" ./configure --with-curl $CUDA $SV2 $HIP >/dev/null
    # configure warns rather than fails on an option it does not know, so ask
    # config.log what it actually took. The alternative is a release that is
    # quietly CPU-only.
    took() { grep -q "^$1='.\+'" config.log; }
    [ -z "$CUDA" ] || took NVCC  || { echo "CUDA was asked for, configure did not take it"; exit 1; }
    [ -z "$HIP" ]  || took HIPCC || { echo "HIP was asked for, configure did not take it"; exit 1; }
    [ -z "$SV2" ]  || took AVIAN_SV2_LIBS || { echo "Stratum v2 was asked for, configure did not take it"; exit 1; }
    make -j"$(nproc)" >/dev/null
    D=$OUT/$NAME-linux-x86_64
    rm -rf "$D"; mkdir -p "$D"
    cp avian-miner "$D/"
    for so in libavian_cuda.so libavian_amd.so; do [ -f "$so" ] && cp "$so" "$D/"; done
    cp "$REPO/README.md" "$REPO/LICENSE" "$REPO/COPYING" "$REPO/doc/licensing.md" "$D/"
    tar -C "$OUT" -czf "$OUT/$NAME-linux-x86_64.tar.gz" "$NAME-linux-x86_64"
    rm -rf "$D"
    echo "  $OUT/$NAME-linux-x86_64.tar.gz"

elif [ "$STAGE" = windows ]; then
    "$REPO/scripts/windows/win-build.sh"
    "$REPO/scripts/windows/win-sv2.sh"
    "$REPO/scripts/windows/win-gpu.sh"
    "$REPO/scripts/windows/win-package.sh"
    # Derived the way win-package.sh derives it, not written out again.
    # Hardcoding the default meant AVIAN_BUILD_DIR moved the build and not
    # this, so a release would have copied whatever an earlier run had left
    # in the default directory: stale, and silently so.
    BUILD_ROOT=${AVIAN_BUILD_DIR:-$(cd "$REPO/.." && pwd)/avian-winbuild}
    REL=${REL:-$BUILD_ROOT/release}
    [ -f "$REL/avian-miner.exe" ] || {
        echo "no package at $REL; AVIAN_BUILD_DIR and this disagree"; exit 1; }
    D=$OUT/$NAME-windows-x64
    rm -rf "$D"; mkdir -p "$D"
    cp "$REL"/* "$D/"
    cp "$REPO/README.md" "$REPO/LICENSE" "$REPO/COPYING" "$REPO/doc/licensing.md" "$D/"
    # Signed in the output directory, after the copy, so nothing rewrites a
    # binary once it carries a signature.
    powershell.exe -ExecutionPolicy Bypass -File \
        "$(cygpath -w "$REPO/scripts/windows/win-sign.ps1")" -Dir "$(cygpath -w "$D")"
    # Compress-Archive rather than zip: MSYS2 ships no zip, and this stage
    # already needs PowerShell for the signing above.
    rm -f "$OUT/$NAME-windows-x64.zip"
    powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$D")' -DestinationPath '$(cygpath -w "$OUT/$NAME-windows-x64.zip")'"
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
    # The same gpg git signs commits with. The key lives in Gpg4win, and
    # neither MSYS2's gpg nor WSL's can see that keyring.
    #
    # Asking git is not enough, because MSYS2 sets HOME to /home/<user> while
    # Git for Windows uses the profile directory, so the two read different
    # gitconfigs and only one of them knows about gpg.program. Reading it
    # from the wrong shell returns nothing, the fallback finds MSYS2's own
    # gpg, and that creates an empty keyring and fails. So: ask git, then
    # look where Gpg4win installs, then ask git again against the profile's
    # own config for the key id.
    GPG=$(git -C "$REPO" config gpg.program 2>/dev/null || true)
    if [ -z "$GPG" ]; then
        for c in "/c/Program Files (x86)/GnuPG/bin/gpg.exe"                  "/c/Program Files/GnuPG/bin/gpg.exe"; do
            [ -x "$c" ] && { GPG=$c; break; }
        done
    fi
    [ -n "$GPG" ] || GPG=gpg
    command -v cygpath >/dev/null && GPG=$(cygpath -u "$GPG" 2>/dev/null || echo "$GPG")

    KEY=$(git -C "$REPO" config user.signingkey 2>/dev/null || true)
    if [ -z "$KEY" ] && [ -n "${USERPROFILE:-}" ] && command -v cygpath >/dev/null; then
        KEY=$(git config --file "$(cygpath -u "$USERPROFILE")/.gitconfig"               user.signingkey 2>/dev/null || true)
    fi

    # Checked before signing rather than after, because the failure this
    # catches is gpg helpfully creating a brand new empty keyring and only
    # then saying it has no key.
    if ! "$GPG" --list-secret-keys ${KEY:+"$KEY"} >/dev/null 2>&1; then
        echo "no secret key${KEY:+ $KEY} in $GPG"
        echo "the signing key is in Gpg4win; run this where git signs commits"
        exit 1
    fi
    echo "signing with $GPG${KEY:+, key $KEY}"
    "$GPG" --armor --detach-sign ${KEY:+--local-user "$KEY"} SHA256SUMS
    echo
    cat SHA256SUMS
    echo
    echo "to publish:"
    echo "  gh release create v$VERSION --title 'avian-miner $VERSION' \\"
    echo "    $(ls -1 "$NAME"-*.tar.gz "$NAME"-*.zip SHA256SUMS SHA256SUMS.asc | tr '\n' ' ')"
fi

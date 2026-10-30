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
#   scripts/release.sh linux     # in WSL, or on Linux; needs docker
#   scripts/release.sh windows   # in MSYS2 MINGW64
#   scripts/release.sh finish    # in MSYS2, once both are present
#
# The linux stage builds in a container and so needs docker, but nothing on
# the host beyond it: no CUDA toolkit, no Rust, not even a GPU. The windows
# stage builds in place and uses $AVIAN_BUILD_DIR, which is what the win-*
# scripts already read. There is no speed reason to move that one, both paths
# being the same NTFS volume, but there is a good one: keeping the build tree
# away from wherever a miner is running, because win-package.sh starts with
# rm -rf.
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
    # Built in an Ubuntu 22.04 container, not against this host. A host build
    # under WSL's 24.04 stamps GLIBC_2.39 into the binary, which excludes every
    # 22.04 and older machine, HiveOS among them; the container's floor is
    # 2.34. Proven on HiveOS 2026-10-30, where the CPU half mined and had a
    # share accepted. For a build against this host, use
    # scripts/configure-build.sh.
    #
    # The container carries no ROCm, so a Linux release has no AMD backend and
    # an AMD owner builds from source. That is the cost of portability and it
    # is paid by few: the backends are dlopened, so the binary is identical for
    # everyone else, and configure now records $ORIGIN in the backend's rpath
    # so a source build can keep the HIP runtime beside it. See doc/building.md.
    #
    # OUT is passed through so both stages land in the directory the finish
    # stage signs. Everything else the container derives for itself, including
    # the version, the package name and the same -O3 -march=x86-64-v2 this
    # stage used to set.
    OUT="$OUT" "$REPO/scripts/linux/build.sh"
    T=$OUT/$NAME-linux-x86_64.tar.gz
    [ -f "$T" ] || { echo "the container produced no $T"; exit 1; }
    echo "  $T"

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

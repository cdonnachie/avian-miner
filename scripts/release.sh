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
    # AVIAN_OCL=1 is what gives a Linux release any AMD support at all, and
    # it is not optional here. The container carries no ROCm, so there is no
    # libavian_amd.so in a release and an AMD owner would otherwise be told to
    # build from source; OpenCL needs only the loader, which the image has, and
    # is the faster of the two backends on every AMD card measured. It also
    # puts avian-ocl-selftest and avian-ocl-batchtest in the tarball, which are
    # how a card that has never run these kernels gets checked on the rig that
    # has it. Without this the Windows release ships the OpenCL backend and the
    # Linux one silently does not. See doc/building.md.
    #
    # OUT is passed through so both stages land in the directory the finish
    # stage signs. Everything else the container derives for itself, including
    # the version, the package name and the same -O3 -march=x86-64-v2 this
    # stage used to set.
    OUT="$OUT" AVIAN_OCL=1 "$REPO/scripts/linux/build.sh"
    T=$OUT/$NAME-linux-x86_64.tar.gz
    [ -f "$T" ] || { echo "the container produced no $T"; exit 1; }
    echo "  $T"

    # The same binaries again under the one layout HiveOS can install, which
    # needs a fixed top-level directory name and the h- scripts beside the
    # miner. A repack rather than a second build, so the two assets cannot
    # differ. See hiveos/ and doc/hiveos.md.
    OUT="$OUT" "$REPO/scripts/linux/hiveos-package.sh" "$T"

elif [ "$STAGE" = windows ]; then
    "$REPO/scripts/windows/win-build.sh"
    "$REPO/scripts/windows/win-sv2.sh"
    # AVIAN_WITH_OPENCL=1 is as little optional here as AVIAN_OCL=1 is in the
    # linux stage above, and for a sharper reason: it is the only AMD backend
    # a Windows user already has the runtime for. avian_amd.dll needs
    # amdhip64.dll out of the HIP SDK, which an AMD owner has no reason to
    # have installed, while the OpenCL.dll it loads instead arrives with every
    # driver. OpenCL is also the faster of the two on every AMD card measured.
    #
    # It keeps the commit stamp honest as a side effect. Without it win-gpu.sh
    # appends the GPU sources to Makefile.am, which is tracked and is not in
    # mkversion.sh's list of files autoreconf rewrites, so every Windows GPU
    # release before this one went out stamped -dirty. With --with-opencl the
    # committed `if HAVE_CUDA` block already names them and nothing is edited.
    AVIAN_WITH_OPENCL=1 "$REPO/scripts/windows/win-gpu.sh"
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
    # avian-webui.exe is already in $REL, copied with everything else above:
    # win-package.sh collects it beside the miner and walks its imports, and
    # win-sign.ps1 signs it, which is the whole reason the dashboard is a C
    # program rather than the script it started as. Only its manual is extra.
    cp "$REPO/doc/webui.md" "$D/"
    # Two batch files that run the miner against the Avian pool once an
    # address is typed into them, for the Windows user who has never
    # opened a terminal. Unsigned: win-sign.ps1 signs binaries, and a
    # batch file is text anyone can read before running it.
    cp "$REPO"/windows/*.bat "$D/"
    # Signed in the output directory, after the copy, so nothing rewrites a
    # binary once it carries a signature.
    powershell.exe -ExecutionPolicy Bypass -File \
        "$(cygpath -w "$REPO/scripts/windows/win-sign.ps1")" -Dir "$(cygpath -w "$D")"
    # Compress-Archive rather than zip: MSYS2 ships no zip, and this stage
    # already needs PowerShell for the signing above.
    rm -f "$OUT/$NAME-windows-x64.zip"
    powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$D")' -DestinationPath '$(cygpath -w "$OUT/$NAME-windows-x64.zip")'"
    echo "  $OUT/$NAME-windows-x64.zip"

    # The installer, from the same directory, when Inno Setup is installed
    # (winget install JRSoftware.InnoSetup). Signed afterwards like the
    # binaries inside it, so what people download carries the signature,
    # not only what it unpacks. Skipped with a line rather than failing,
    # so a machine without Inno still produces the zip.
    ISCC="/c/Program Files (x86)/Inno Setup 6/ISCC.exe"
    if [ -x "$ISCC" ]; then
        # VersionInfoVersion takes digits and dots only, so a pre-release
        # tag such as 1.3.0-rc1 is cut back to 1.3.0 for that field alone.
        NUMERIC=$(printf '%s' "$VERSION" | sed -E 's/[^0-9.].*$//')
        rm -f "$OUT/$NAME-windows-x64-setup.exe"
        "$ISCC" /Q "/DAppVersion=$VERSION" "/DNumericVersion=$NUMERIC" \
            "/DSourceDir=$(cygpath -w "$D")" "/DOutputDir=$(cygpath -w "$OUT")" \
            "$(cygpath -w "$REPO/scripts/windows/avian-miner.iss")"
        powershell.exe -ExecutionPolicy Bypass -File \
            "$(cygpath -w "$REPO/scripts/windows/win-sign.ps1")" \
            -Files "$(cygpath -w "$OUT/$NAME-windows-x64-setup.exe")"
        echo "  $OUT/$NAME-windows-x64-setup.exe"
    else
        echo "  (no Inno Setup at $ISCC, so no setup.exe; winget install JRSoftware.InnoSetup)"
    fi
    rm -rf "$D"

else
    cd "$OUT"
    ls -1 "$NAME"-*.tar.gz "$NAME"-*.zip >/dev/null 2>&1 || {
        echo "nothing to finish: run the linux and windows stages first"; exit 1; }
    # The HiveOS package is $NAME.tar.gz with nothing after the version,
    # because HiveOS parses the miner's name and version out of the file name
    # and any suffix breaks the install. Every glob here is "$NAME-*", so it
    # would be left out of SHA256SUMS and so out of the signature: a
    # published asset nobody can verify, which is worse than one that is
    # missing, and silent either way. Named explicitly, and only when it is
    # there, so a release without it still finishes.
    HIVE=
    [ -f "$NAME.tar.gz" ] && HIVE=$NAME.tar.gz
    # The installer is optional in the same way: built only where Inno
    # Setup is, and named here only when it is there.
    SETUP=
    [ -f "$NAME-windows-x64-setup.exe" ] && SETUP=$NAME-windows-x64-setup.exe
    sha256sum $HIVE "$NAME"-*.tar.gz "$NAME"-*.zip $SETUP > SHA256SUMS
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
    # -R, because this hint is pasted in $OUT and $OUT is not a git checkout,
    # so gh has no remote to infer the repository from and stops with "none of
    # the git remotes configured for this repository point to a known GitHub
    # host". Derived rather than written out, so a fork gets its own name.
    SLUG=$(git -C "$REPO" remote get-url origin 2>/dev/null |
           sed -E 's|^.*[:/]([^/]+/[^/]+)$|\1|; s|\.git$||')
    echo "  gh release create v$VERSION ${SLUG:+-R $SLUG }\\"
    echo "    --title 'avian-miner $VERSION' \\"
    # $HIVE first and by name, for the same reason as SHA256SUMS above:
    # every glob here is "$NAME-*" and the HiveOS package has nothing
    # after the version. Leaving it out would publish a SHA256SUMS that
    # lists a file the release does not contain.
    echo "    $(ls -1 $HIVE "$NAME"-*.tar.gz "$NAME"-*.zip $SETUP SHA256SUMS SHA256SUMS.asc | tr '\n' ' ')"
fi

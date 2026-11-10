#!/bin/bash
# Fetch the two signed PawnIO modules avian-sensors loads into windows/pawnio,
# where win-package.sh picks them up.
#
#   scripts/windows/fetch-pawnio-modules.sh
#
# Pinned to one release of https://github.com/namazso/PawnIO.Modules and to
# the SHA-256 GitHub computed for its zip, so what lands in the package is
# exactly what was looked at when this was written, and a changed download
# is refused rather than shipped. The modules are LGPL-2.1 and used as
# published: PawnIO checks their signature and refuses an altered one, which
# is also why nothing here touches their contents. To move to a newer
# release, change the three values below and read its notes.
set -eu

TAG=0.2.11
ZIP=release_0_2_11.zip
SHA256=43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4
URL="https://github.com/namazso/PawnIO.Modules/releases/download/$TAG/$ZIP"
WANT="AMDFamily17.bin IntelMSR.bin"

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DEST=$REPO/windows/pawnio
mkdir -p "$DEST"

have=1
for f in $WANT; do [ -f "$DEST/$f" ] || have=0; done
if [ $have = 1 ]; then
    echo "pawnio modules already in $DEST:"
    for f in $WANT; do echo "  $f"; done
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "fetching PawnIO.Modules $TAG"
curl -fsSL --retry 3 -o "$WORK/$ZIP" "$URL"
echo "$SHA256  $WORK/$ZIP" | sha256sum -c - >/dev/null || {
    echo "checksum mismatch on $ZIP: not the release this script was pinned to" >&2
    exit 1
}

# Whichever unzipper this shell has: unzip on most Linux boxes and WSL,
# python nearly everywhere, PowerShell on the MSYS2 build host where unzip
# is not installed by default. The release is built from MSYS2, but the
# modules get fetched from wherever someone happens to be sitting.
mkdir -p "$WORK/x"
if command -v unzip >/dev/null 2>&1; then
    unzip -q "$WORK/$ZIP" -d "$WORK/x"
elif command -v python3 >/dev/null 2>&1; then
    python3 -m zipfile -e "$WORK/$ZIP" "$WORK/x"
elif command -v python >/dev/null 2>&1; then
    python -m zipfile -e "$WORK/$ZIP" "$WORK/x"
elif command -v powershell.exe >/dev/null 2>&1 && command -v cygpath >/dev/null 2>&1; then
    powershell.exe -NoProfile -Command \
        "Expand-Archive -LiteralPath '$(cygpath -w "$WORK/$ZIP")' -DestinationPath '$(cygpath -w "$WORK/x")' -Force"
else
    echo "no unzip, python or PowerShell here to open $ZIP with" >&2
    exit 1
fi

for f in $WANT; do
    found=$(find "$WORK/x" -type f -name "$f" | head -n 1)
    [ -n "$found" ] || { echo "$f is not in $ZIP; the release layout changed" >&2; exit 1; }
    cp "$found" "$DEST/$f"
    echo "  $f ($(stat -c %s "$DEST/$f") bytes)"
done
echo "into $DEST"

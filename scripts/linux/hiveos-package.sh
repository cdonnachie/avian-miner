#!/bin/bash
# Build the HiveOS custom-miner package from the portable Linux tarball.
#
#   scripts/linux/hiveos-package.sh [path/to/avian-miner-VER-linux-x86_64.tar.gz]
#
# HiveOS installs a custom miner by downloading one tarball, and it takes the
# miner's identity from the *file name*, not from h-manifest.conf. Measured
# on a rig: it splits the basename at the LAST hyphen, treats everything
# before it as the name and the rest as the version, and then expects the
# archive to hold one top-level directory of exactly that name, which it
# unpacks to /hive/miners/custom/<name>/.
#
# So the file must be named avian-miner-<version>.tar.gz and the directory
# inside it avian-miner, and the version must contain no hyphen of its own:
# avian-miner-1.1.0-dev.tar.gz would be read as the miner "avian-miner-1.1.0"
# at version "dev" and fail to find its directory. Hyphens are therefore
# dropped from the version here, which affects nothing else because the
# miner's own version comes from the binary.
#
# This is also why the portable tarball cannot serve as the HiveOS one: its
# name carries -linux-x86_64 and its directory carries the version. Same
# binaries, repacked, rather than a second build.
set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MINER=$(cd "$HERE/../.." && pwd)
NAME=avian-miner

VERSION=$(sed -n 's/^AC_INIT(\[avian-miner\], \[\(.*\)\])/\1/p' \
              "$MINER/configure.ac")
[ -n "$VERSION" ] || { echo "no version in configure.ac"; exit 1; }

# The version as HiveOS can parse it, which is the version without hyphens.
HIVE_VERSION=$(printf '%s' "$VERSION" | tr -d '-')
case "$HIVE_VERSION" in
    *-*|"") echo "cannot make a HiveOS version out of $VERSION"; exit 1 ;;
esac

OUT=${OUT:-$(cd "$MINER/.." && pwd)/avian-miner-release/container}
SRC=${1:-$OUT/$NAME-$VERSION-linux-x86_64.tar.gz}
[ -f "$SRC" ] || { echo "no Linux tarball at $SRC"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

tar -xzf "$SRC" -C "$WORK"
INNER=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d)
[ -d "$INNER" ] || { echo "$SRC has no single top-level directory"; exit 1; }
mv "$INNER" "$WORK/$NAME"

# The miner has to be there under that exact name, because h-run.sh execs it
# by name and a silent rename would only show up as a dead flight sheet.
[ -x "$WORK/$NAME/avian-miner" ] || { echo "no avian-miner in $SRC"; exit 1; }

for f in h-manifest.conf h-config.sh h-run.sh h-stats.sh; do
    cp "$MINER/hiveos/$f" "$WORK/$NAME/$f"
done
chmod +x "$WORK/$NAME"/h-*.sh

# The manifest says the same version HiveOS parsed out of the file name, so
# the rig page and the package agree. The binary's own --version still
# reports the full one, hyphen and all.
sed -i "s/@VERSION@/$HIVE_VERSION/" "$WORK/$NAME/h-manifest.conf"

PKG=$OUT/$NAME-$HIVE_VERSION.tar.gz
mkdir -p "$OUT"
tar -czf "$PKG" -C "$WORK" "$NAME"

echo "  $PKG"
echo "    HiveOS will read this as miner \"$NAME\" version \"$HIVE_VERSION\""
tar -tzf "$PKG" | sed -n '1,12p' | sed 's/^/    /'

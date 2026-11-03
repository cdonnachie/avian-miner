#!/bin/bash
# Build the HiveOS custom-miner package from the portable Linux tarball.
#
#   scripts/linux/hiveos-package.sh [path/to/avian-miner-VER-linux-x86_64.tar.gz]
#
# HiveOS installs a custom miner by downloading one tarball and unpacking it
# into /hive/miners/custom/, so the archive must contain exactly one top-level
# directory and that directory's name must equal CUSTOM_NAME in
# h-manifest.conf. The portable tarball's directory carries the version, so it
# cannot serve: this repacks it under the fixed name with the four h- scripts
# added, rather than building anything a second time.
set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MINER=$(cd "$HERE/../.." && pwd)
NAME=avian-miner

VERSION=$(sed -n 's/^AC_INIT(\[avian-miner\], \[\(.*\)\])/\1/p' \
              "$MINER/configure.ac")
[ -n "$VERSION" ] || { echo "no version in configure.ac"; exit 1; }

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

# One version, from configure.ac, so the manifest cannot drift from the
# binary it ships beside.
sed -i "s/@VERSION@/$VERSION/" "$WORK/$NAME/h-manifest.conf"

PKG=$OUT/$NAME-$VERSION-hiveos.tar.gz
mkdir -p "$OUT"
tar -czf "$PKG" -C "$WORK" "$NAME"

echo "  $PKG"
tar -tzf "$PKG" | sed -n '1,12p' | sed 's/^/    /'

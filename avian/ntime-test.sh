#!/bin/bash
# The nTime rolling rule, on its own. Two source files, the library's header,
# no miner objects, no node. See avian/ntime-test.c for why this is the only
# place the rule can be checked.
#
# Usage: avian/ntime-test.sh [path-to-avian-sv2]
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/.." && pwd)"
SV2="${1:-/mnt/c/development/avian-sv2}"
OUT="${TMPDIR:-/tmp}/avian-ntime-test-$$"
trap 'rm -f "$OUT"' EXIT

gcc -O2 -std=gnu11 -Wall -Wextra -Werror \
    -I"$TREE" -I"$SV2/crates/avian-sv2/include" \
    -o "$OUT" "$HERE/ntime-test.c" "$HERE/ntime.c"
"$OUT"

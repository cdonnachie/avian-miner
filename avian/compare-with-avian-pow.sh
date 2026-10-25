#!/bin/bash
# Does this miner's MinotaurX produce the same hash as Avian's own code?
#
# `cpuminer -a minotaurx --avian-selftest` answers the weaker version of this,
# against hashes a real node reported. This answers the stronger one, against
# Avian Core's implementation directly, on arbitrary headers rather than two
# known blocks. avian-pow in the avian-sv2 workspace FFIs src/algo/minotaurx,
# so the comparison is against the node's code and not a reimplementation.
#
# Two processes, on purpose. avian-pow and this miner both vendor yespower and
# export the same symbol names, so a single binary containing both resolves one
# side's call into the other's code and agreement would prove nothing. That is
# not hypothetical: the first attempt segfaulted with a backtrace running from
# Avian's Minotaurx into cpuminer's yespower. Only hex strings cross between
# the two processes here.
#
# Needs the miner already built in this tree, and the avian-sv2 workspace.
# Usage: avian/compare-with-avian-pow.sh [path-to-avian-sv2]
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
MINER_TREE="$(cd "$HERE/.." && pwd)"
SV2="${1:-/mnt/c/development/avian-sv2}"
PROBE="${TMPDIR:-/tmp}/minotaur-probe-$$"
trap 'rm -f "$PROBE"' EXIT

[ -d "$SV2/crates/avian-pow" ] || { echo "no avian-pow under $SV2"; exit 2; }

echo "== building the miner-side probe"
cd "$MINER_TREE"
# Every object the build produced except the ones that belong to the mining
# loop; minotaur-probe-stubs.c stands in for what those defined. Linking the
# whole miner would drag in main.
OBJS=$(find . -name "cpuminer-*.o" \
       ! -name "cpuminer-cpu-miner.o" ! -name "cpuminer-dummy.o" \
       ! -name "cpuminer-api.o" ! -name "cpuminer-util.o" \
       ! -name "cpuminer-sysinfos.o" ! -name "cpuminer-algo-gate-api.o" \
       ! -name "cpuminer-sv2.o" ! -name "cpuminer-selftest.o" | tr "\n" " ")
[ -n "$OBJS" ] || { echo "nothing built here; run make first"; exit 2; }

# CPPFLAGS and LDFLAGS carry through, because miner.h includes curl/curl.h and
# curl is not always under /usr/include.
gcc -O2 -march=native -std=gnu11 -Wall -I. -I./compat/jansson \
    ${CPPFLAGS:-} ${LDFLAGS:-} \
    -o "$PROBE" "$HERE/minotaur-probe.c" "$HERE/minotaur-probe-stubs.c" $OBJS \
    -lstdc++ -lpthread -ldl -lm -lgmp -lz -lcurl 2>&1 \
    | grep -v "GNU-stack\|deprecated" || true
[ -x "$PROBE" ] || { echo "the probe did not link"; exit 2; }

echo "== building the Avian-side printer"
cd "$SV2"
cargo build -q --example hash_headers -p avian-pow

# Real regtest blocks: the header the node accepted, and the hash it reported,
# in the order either implementation returns rather than the reversed order a
# block hash is printed in.
H1=00000132779b12392b5348809aea7c9c35e5b46928896ae4c686c017a41e16639db006660da3727a04ccf5adfde3ae4cac4770f54e0a37cc482f6699f7dee442f8c68ab37d31de6affff7f2000000000
N1=cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52effd23
H2=00000132cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52effd235e18e85e588e466a3689d4f3b675f00dac4998ed309bc84cfd55038775bd04e77e31de6affff7f2000000000
N2=bee9620fc38ff31b6f009a13a0c66e17b7c55ec8de79554429ce704d769b927a

# And headers that are not real blocks, so agreement cannot be an artefact of
# two vectors. The nonce is the header's last four bytes.
HEADERS=("$H1" "$H2")
for n in $(seq 1 64); do
  HEADERS+=("${H1:0:152}$(printf '%08x' "$n")")
done

AVIAN=$("$SV2/target/debug/examples/hash_headers" minotaurx "${HEADERS[@]}")
MINER=$("$PROBE" "${HEADERS[@]}")

echo
echo "== the two real blocks"
fail=0
i=0
for node in "$N1" "$N2"; do
  i=$((i + 1))
  a=$(echo "$AVIAN" | sed -n "${i}p")
  m=$(echo "$MINER" | sed -n "${i}p")
  echo "block $i"
  echo "  node   $node"
  echo "  avian  $a"
  echo "  miner  $m"
  [ "$a" = "$node" ] || { echo "  AVIAN DISAGREES WITH THE NODE"; fail=1; }
  [ "$m" = "$a" ]    || { echo "  MINER DISAGREES WITH AVIAN"; fail=1; }
done

echo
echo "== all ${#HEADERS[@]} headers, including 64 swept nonces"
if diffs=$(diff <(echo "$AVIAN") <(echo "$MINER")); then
  echo "agree byte for byte"
else
  echo "DISAGREEMENT:"
  echo "$diffs" | head -20
  fail=1
fi

echo
[ "$fail" -eq 0 ] || { echo "FAIL"; exit 1; }
echo "OK: this miner's MinotaurX agrees with Avian's, and Avian's with the node"

#!/bin/bash
# Does this miner's proof of work produce the same hash as Avian's own code?
#
# `cpuminer --avian-selftest` answers the weaker version of this, against
# hashes a real node reported. This answers the stronger one, against Avian
# Core's implementation directly, on headers no node ever saw. avian-pow in the
# avian-sv2 workspace FFIs src/algo, so the comparison is against the node's
# code and not a reimplementation.
#
# Two processes, on purpose. avian-pow and this miner both vendor yespower and
# export the same symbol names, so a single binary containing both resolves one
# side's call into the other's code and agreement would prove nothing. That is
# not hypothetical: the first attempt segfaulted with a backtrace running from
# Avian's Minotaurx into cpuminer's yespower. Only hex strings cross here.
#
# X16RT needs more than MinotaurX did. Its hash chain comes from
# SHA256d(nTime & 0xffffff80), so the algorithm changes every 128 seconds and a
# sweep inside one epoch exercises one chain while looking exactly like a
# thorough check. So this sweeps nTime across epoch boundaries as well as
# nonces, and counts the distinct seeds it actually covered rather than
# assuming it covered any.
#
# Needs the miner already built in this tree, and the avian-sv2 workspace.
# Usage: avian/compare-with-avian-pow.sh [x16rt|minotaurx|both] [path-to-avian-sv2]
set -eu

WHICH="${1:-both}"
# The sibling clone, derived rather than named so this works from
# any checkout. The argument still overrides it.
SV2="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/avian-sv2}"
HERE="$(cd "$(dirname "$0")" && pwd)"
MINER_TREE="$(cd "$HERE/.." && pwd)"
PROBE="${TMPDIR:-/tmp}/pow-probe-$$"
trap 'rm -f "$PROBE"' EXIT

[ -d "$SV2/crates/avian-pow" ] || { echo "no avian-pow under $SV2"; exit 2; }

echo "== building the miner-side probe"
cd "$MINER_TREE"
# Every object the build produced except the ones belonging to the mining loop;
# pow-probe-stubs.c stands in for what those defined. Linking the whole miner
# would drag in main.
OBJS=$(find . -name "avian_miner-*.o" \
       ! -name "avian_miner-cpu-miner.o" ! -name "avian_miner-dummy.o" \
       ! -name "avian_miner-api.o" ! -name "avian_miner-util.o" \
       ! -name "avian_miner-sysinfos.o" ! -name "avian_miner-algo-gate-api.o" \
       ! -name "avian_miner-sv2.o" ! -name "avian_miner-selftest.o" | tr "\n" " ")
[ -n "$OBJS" ] || { echo "nothing built here; run make first"; exit 2; }

# CPPFLAGS and LDFLAGS carry through, because miner.h includes curl/curl.h and
# curl is not always under /usr/include.
gcc -O2 -march=native -std=gnu11 -Wall -I. -I./compat/jansson \
    ${CPPFLAGS:-} ${LDFLAGS:-} \
    -o "$PROBE" "$HERE/pow-probe.c" "$HERE/pow-probe-stubs.c" $OBJS \
    -lstdc++ -lpthread -ldl -lm -lgmp -lz -lcurl 2>&1 \
    | grep -v "GNU-stack\|deprecated\|DT_TEXTREL" || true
[ -x "$PROBE" ] || { echo "the probe did not link"; exit 2; }

echo "== building the Avian-side printer"
cd "$SV2"
cargo build -q --example hash_headers -p avian-pow
AVIAN_BIN="$SV2/target/debug/examples/hash_headers"

# Real blocks a regtest node produced and accepted, captured with
# avian/capture-vectors.sh, which uses setmocktime to put each X16RT block in
# its own 128-second epoch. The hashes are in the order the hash functions
# return them, not the reversed order a block hash prints in.
MINOTAURX_HEADERS=(
  00000132779b12392b5348809aea7c9c35e5b46928896ae4c686c017a41e16639db006660da3727a04ccf5adfde3ae4cac4770f54e0a37cc482f6699f7dee442f8c68ab37d31de6affff7f2000000000
  00000132cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52effd235e18e85e588e466a3689d4f3b675f00dac4998ed309bc84cfd55038775bd04e77e31de6affff7f2000000000
)
MINOTAURX_HASHES=(
  cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52effd23
  bee9620fc38ff31b6f009a13a0c66e17b7c55ec8de79554429ce704d769b927a
)

X16RT_HEADERS=(
  000000227f78cf1d89b20dbe77361d6a87e47bad06399047dda5abe884ed273dd0343665a784fe96fd3d88f75a19f7583789078768670177ec0490ac6e5acd6f850296418086de6affff7f2001000000
  0000003287a28dc4b555f3305df832ed3962605f2968329e08e97e8a7841cc5cba4150537aefb40130d830654833e0aa644184421a11d3c59c2728e20c0fe1693a7b7a8f0087de6affff7f2000000000
  00000032f708b4f7ef90d66f058ad25e7b992e9b69d233716eac09d6a608f7a6046f782d70e29a76e8c88e79f21f23dce548d3e26a1acba9bc0e5f71f0a89b850be1f64f8087de6affff7f2000000000
  00000032cb1eb367e7ce8681c681c398f388c1490ef93714ed73421751dfa54fecb8a535b43951aa433bf59042d773b0bf99b7be72a74a3c0e340d9372b7fa7ef6edfd7f0088de6affff7f2001000000
  0000003241a125d8a0aba5c37d57b54699a25379af86c752660b56a84097efa463d2a40604e9e803b1c55f24b8636d02ea66ddf97e5732d921e1318403ee1cef883809eb8088de6affff7f2000000000
  000000324792c016028888c222d2d46e3bdac513752315baa541544b526fd47550cb7f22b17f6bef8f58071e92bdbd82e6ebbd6b3de0ea86cf4b4cf541ff4e076871906b0089de6affff7f2004000000
)
X16RT_HASHES=(
  87a28dc4b555f3305df832ed3962605f2968329e08e97e8a7841cc5cba415053
  f708b4f7ef90d66f058ad25e7b992e9b69d233716eac09d6a608f7a6046f782d
  cb1eb367e7ce8681c681c398f388c1490ef93714ed73421751dfa54fecb8a535
  41a125d8a0aba5c37d57b54699a25379af86c752660b56a84097efa463d2a406
  4792c016028888c222d2d46e3bdac513752315baa541544b526fd47550cb7f22
  f73cd553a1b33f4b21c6002e3e7cc525a2300f7d82b44f8bcd56414ac25b672f
)
# The nTime of the first X16RT vector, so the sweep starts where a real block
# was rather than at an arbitrary instant.
X16RT_BASE_NTIME=1792968320

# A 32-bit value as four little-endian hex bytes.
le32() { local v; v=$(printf '%08x' "$1"); echo "${v:6:2}${v:4:2}${v:2:2}${v:0:2}"; }
# nTime is the header's 69th through 72nd byte, the nonce its last four.
set_ntime() { echo "${1:0:136}$(le32 "$2")${1:144}"; }
set_nonce() { echo "${1:0:152}$(le32 "$2")"; }

fail=0

compare() {
    local algo=$1
    shift
    local -a headers=("$@")
    local avian miner diffs

    avian=$("$AVIAN_BIN" "$algo" "${headers[@]}")
    miner=$("$PROBE" "$algo" "${headers[@]}")
    if diffs=$(diff <(echo "$avian") <(echo "$miner")); then
        echo "  ok   all ${#headers[@]} headers agree byte for byte"
    else
        echo "  FAIL disagreement:"
        echo "$diffs" | head -20
        fail=1
    fi
}

check_anchors() {
    local algo=$1
    local -n hdrs=$2
    local -n want=$3
    local avian a i ok=1

    avian=$("$AVIAN_BIN" "$algo" "${hdrs[@]}")
    for ((i = 0; i < ${#want[@]}; i++)); do
        a=$(echo "$avian" | sed -n "$((i + 1))p")
        if [ "$a" != "${want[$i]}" ]; then
            echo "  FAIL block $((i + 1)): avian $a, node ${want[$i]}"
            ok=0
            fail=1
        fi
    done
    [ "$ok" -eq 1 ] \
        && echo "  ok   Avian agrees with the node on all ${#want[@]} real blocks"
}

run_minotaurx() {
    local -a headers
    local n
    echo
    echo "== minotaurx"
    check_anchors minotaurx MINOTAURX_HEADERS MINOTAURX_HASHES
    headers=("${MINOTAURX_HEADERS[@]}")
    for n in $(seq 1 64); do
        headers+=("$(set_nonce "${MINOTAURX_HEADERS[0]}" "$n")")
    done
    compare minotaurx "${headers[@]}"
    echo "  note MinotaurX has no epochs, so there is no nTime sweep to do"
}

run_x16rt() {
    local -a headers ntimes
    local n e epoch t seeds distinct
    echo
    echo "== x16rt"
    check_anchors x16rt X16RT_HEADERS X16RT_HASHES

    headers=("${X16RT_HEADERS[@]}")
    ntimes=()

    for n in $(seq 1 64); do
        headers+=("$(set_nonce "${X16RT_HEADERS[0]}" "$n")")
    done

    # nTime across epoch boundaries. The points either side of each boundary
    # are the ones that matter: a miner rolling nTime one second too far
    # computes a different function, silently.
    for e in $(seq 0 5); do
        epoch=$((X16RT_BASE_NTIME + e * 128))
        for t in $((epoch - 1)) "$epoch" $((epoch + 1)) $((epoch + 64)) \
                 $((epoch + 126)) $((epoch + 127)); do
            ntimes+=("$t")
            headers+=("$(set_ntime "${X16RT_HEADERS[0]}" "$t")")
        done
    done

    compare x16rt "${headers[@]}"

    # How many distinct hash chains that actually exercised. Without counting,
    # the sweep could have sat inside one epoch and proved almost nothing.
    seeds=$("$AVIAN_BIN" seeds "${ntimes[@]}")
    distinct=$(echo "$seeds" | sort -u | wc -l)
    echo "  ok   the nTime sweep covered $distinct distinct X16RT seeds across ${#ntimes[@]} nTimes"
    if [ "$distinct" -lt 5 ]; then
        echo "  FAIL fewer than 5 epochs, so the sweep is not testing what it claims"
        fail=1
    fi
}

case "$WHICH" in
  minotaurx) run_minotaurx ;;
  x16rt)     run_x16rt ;;
  both)      run_minotaurx; run_x16rt ;;
  *) echo "usage: $0 [x16rt|minotaurx|both] [path-to-avian-sv2]"; exit 2 ;;
esac

echo
[ "$fail" -eq 0 ] || { echo "FAIL"; exit 1; }
echo "OK: this miner agrees with Avian's own code, and Avian's with the node"

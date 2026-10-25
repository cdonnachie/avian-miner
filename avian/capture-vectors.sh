#!/bin/bash
# Captures proof-of-work test vectors from a real Avian node.
#
# The hardcoded vectors in selftest.c and compare-with-avian-pow.sh came from
# here, so they are auditable rather than magic. For X16RT it matters that the
# blocks land in *different* 128-second epochs, because the hash chain is
# derived from SHA256d(nTime & 0xffffff80): vectors inside one epoch all
# exercise one seed and would look exactly like a thorough check. setmocktime
# is what puts them in different ones.
#
# Usage: avian/capture-vectors.sh <x16rt|minotaurx> [blocks] [path-to-bin]
set -eu

ALGO="${1:-x16rt}"
BLOCKS="${2:-6}"
BIN="${3:-$HOME/avian-build/bin}"
AVIAND="$BIN/aviand"
CLI="$BIN/avian-cli"

free_port() {
    local base=$1 port
    for port in $(seq "$base" $((base + 200))); do
        ss -ltn 2>/dev/null | grep -qE "[:.]$port\b" || { echo "$port"; return; }
    done
    echo "$base"
}

RPC_PORT=$(free_port 29632)
DATADIR="${TMPDIR:-/tmp}/avian-vectors-$$"
NODE_PID=""

rpc() { "$CLI" -regtest "-datadir=$DATADIR" "-rpcport=$RPC_PORT" -rpcuser=v \
               -rpcpassword=v "$@" 2>/dev/null; }

cleanup() {
    [ -n "$NODE_PID" ] && rpc stop >/dev/null 2>&1
    sleep 1
    [ -n "$NODE_PID" ] && kill -9 "$NODE_PID" 2>/dev/null
    rm -rf "$DATADIR"
    wait 2>/dev/null
}
trap cleanup EXIT

mkdir -p "$DATADIR"
"$AVIAND" -regtest "-datadir=$DATADIR" "-rpcport=$RPC_PORT" -rpcuser=v \
    -rpcpassword=v -listen=0 "-powalgo=$ALGO" > "$DATADIR/node.log" 2>&1 &
NODE_PID=$!

for _ in $(seq 1 60); do
    rpc getblockcount >/dev/null 2>&1 && break
    sleep 1
done
rpc getblockcount >/dev/null 2>&1 || { echo "the node never came up"; exit 1; }

rpc createwallet v >/dev/null 2>&1 || rpc loadwallet v >/dev/null 2>&1
ADDR=$(rpc getnewaddress "" legacy)

# Start on a 128-second boundary and step a whole epoch each time, so every
# block gets a different X16RT seed.
BASE=$(( ($(date +%s) / 128) * 128 ))

echo "# $ALGO vectors from a regtest node, one per 128-second epoch"
echo "# header (80 bytes) then the hash the node reported, in the order the"
echo "# hash functions return it rather than the reversed order it prints in"
for i in $(seq 0 $((BLOCKS - 1))); do
    T=$((BASE + i * 128))
    rpc setmocktime "$T" >/dev/null
    rpc -named generatetoaddress nblocks=1 address="$ADDR" powalgo="$ALGO" \
        >/dev/null
    HASH=$(rpc getbestblockhash)
    HEADER=$(rpc getblockheader "$HASH" false)
    POWTYPE=$(rpc getblock "$HASH" 1 \
              | grep -o '"powtype": *"[^"]*"' | sed 's/.*"\([^"]*\)"$/\1/')
    # A block hash prints reversed; undo that.
    INTERNAL=$(echo "$HASH" | fold -w2 | tac | tr -d '\n')
    NTIME_LE=${HEADER:136:8}
    # nTime is the header's 69th through 72nd byte, little-endian.
    NTIME=$(printf "%d" "0x${NTIME_LE:6:2}${NTIME_LE:4:2}${NTIME_LE:2:2}${NTIME_LE:0:2}")
    EPOCH=$(( NTIME & 0xffffff80 ))
    echo
    echo "# powtype $POWTYPE, ntime $NTIME, epoch $EPOCH"
    echo "header $HEADER"
    echo "hash   $INTERNAL"
done

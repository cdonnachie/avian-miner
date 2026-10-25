#!/bin/bash
# The milestone, asserted rather than observed:
#
#   aviand -> pool -> cpuminer-avian -> pool -> aviand
#
# A real MinotaurX block, mined by this miner over Stratum v2, accepted by the
# node, with getblock reporting minotaurx and the coinbase paying the address
# the miner asked to be paid at. Each of those four is a separate assertion,
# because three of them can pass while the fourth quietly does not.
#
# Loopback only, regtest only. Usage:
#
#   avian/regtest-e2e.sh [path-to-avian-build/bin] [path-to-avian-sv2]
#
# ALGO=x16rt or ALGO=minotaurx (default) picks the algorithm, end to end:
# the node's -sv2powalgo, the pool's --algo and the miner's -a all follow it,
# and the powtype assertion checks the node agrees.
# CHANNEL=standard asks for a standard channel, RECONNECT=1 also restarts
# the pool under the miner.
set -u

ALGO="${ALGO:-minotaurx}"
case "$ALGO" in
  x16rt|minotaurx) ;;
  *) echo "ALGO must be x16rt or minotaurx, not $ALGO"; exit 2 ;;
esac

BIN="${1:-$HOME/avian-build/bin}"
SV2="${2:-/mnt/c/development/avian-sv2}"
AVIAND="$BIN/aviand"
CLI="$BIN/avian-cli"
MINER="$(cd "$(dirname "$0")/.." && pwd)/cpuminer"

# Free ports rather than fixed ones, so this does not collide with whatever
# else on the machine already holds a port in the usual regtest range.
free_port() {
    local base=$1
    local port
    for port in $(seq "$base" $((base + 200))); do
        if ! ss -ltn 2>/dev/null | grep -qE "[:.]$port\\b"; then
            echo "$port"
            return
        fi
    done
    echo "$base"
}

RPC_PORT=$(free_port 29432)
SV2_PORT=$(free_port $((RPC_PORT + 1)))
MINING_PORT=$(free_port $((SV2_PORT + 1)))
DATADIR="${TMPDIR:-/tmp}/cpuminer-avian-e2e-$$"
LOGDIR="$DATADIR/logs"
NODE_PID=""
POOL_PID=""
MINER_PID=""

rpc() { "$CLI" -regtest "-datadir=$DATADIR" "-rpcport=$RPC_PORT" -rpcuser=e2e \
               -rpcpassword=e2e "$@" 2>/dev/null; }

cleanup() {
    for pid in "$MINER_PID" "$POOL_PID"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    [ -n "$NODE_PID" ] && rpc stop >/dev/null 2>&1
    sleep 1
    for pid in "$MINER_PID" "$POOL_PID" "$NODE_PID"; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    done
    wait 2>/dev/null
}
trap cleanup EXIT

fail() { echo; echo "FAIL: $*"; exit 1; }

for tool in "$AVIAND" "$CLI" "$MINER"; do
    [ -x "$tool" ] || fail "$tool is not executable"
done

mkdir -p "$LOGDIR"

echo "== starting aviand on regtest, serving $ALGO"
# -listen=0 because the P2P port is not wanted and binding it can collide with
# another regtest node on the same machine.
"$AVIAND" -regtest "-datadir=$DATADIR" "-rpcport=$RPC_PORT" -rpcuser=e2e \
    -rpcpassword=e2e -listen=0 -fallbackfee=0.0001 \
    -sv2 "-sv2bind=127.0.0.1:$SV2_PORT" "-sv2powalgo=$ALGO" \
    -sv2maxrejects=1000 -debug=sv2 > "$LOGDIR/node.log" 2>&1 &
NODE_PID=$!

for _ in $(seq 1 60); do
    rpc getblockcount >/dev/null 2>&1 && break
    sleep 1
done
rpc getblockcount >/dev/null 2>&1 || fail "the node never came up; see $LOGDIR/node.log"

# Addresses come from a wallet, and a fresh datadir has none.
rpc createwallet e2e >/dev/null 2>&1 || rpc loadwallet e2e >/dev/null 2>&1

# A node with no blocks refuses templates silently, so prime the chain. This
# block is mined by the node itself, not by us.
PRIME_ADDR=$(rpc getnewaddress "" legacy)
[ -n "$PRIME_ADDR" ] || fail "could not get an address from the node"
rpc generatetoaddress 1 "$PRIME_ADDR" >/dev/null || fail "could not prime the chain"
BEFORE=$(rpc getblockcount)
echo "   height $BEFORE"

# Who the miner wants paying. A legacy address, because the miner's own
# address_to_script handles P2PKH for any version byte but its bech32 decoder
# only knows Bitcoin's prefixes.
PAYOUT=$(rpc getnewaddress "" legacy)
[ -n "$PAYOUT" ] || fail "could not get a payout address"
echo "   the miner will ask to be paid at $PAYOUT"

echo "== starting the pool"
# Space-separated, not --opt=value: that is what this pool's parser accepts.
"$SV2/target/release/avian-sv2-pool" \
    --datadir "$DATADIR" --chain regtest \
    --connect "127.0.0.1:$SV2_PORT" \
    --mining-bind "127.0.0.1:$MINING_PORT" \
    --pool-key "$DATADIR/pool-authority-key" \
    --coinbase-tag "Sv2/cpuminer-avian e2e//" \
    --algo "$ALGO" > "$LOGDIR/pool.log" 2>&1 &
POOL_PID=$!

POOL_KEY=""
for _ in $(seq 1 60); do
    POOL_KEY=$(grep -A1 "hex (32-byte x-only)" "$LOGDIR/pool.log" 2>/dev/null \
               | tail -1 | tr -d " \t\r")
    [ ${#POOL_KEY} -eq 64 ] && break
    sleep 1
done
[ ${#POOL_KEY} -eq 64 ] || fail "the pool never printed its authority key; see $LOGDIR/pool.log"
echo "   authority $POOL_KEY"

grep -q "serving" "$LOGDIR/pool.log" 2>/dev/null || sleep 2

# Standard channels have to keep working, so this runs either way:
#   CHANNEL=standard avian/regtest-e2e.sh
CHANNEL_ARG=""
if [ "${CHANNEL:-extended}" = "standard" ]; then
    CHANNEL_ARG="--sv2-standard"
fi

echo "== starting cpuminer-avian over Stratum v2 ($ALGO, ${CHANNEL:-extended} channel)"
"$MINER" -a "$ALGO" -t 4 $CHANNEL_ARG \
    "--sv2=127.0.0.1:$MINING_PORT" \
    "--sv2-authority=$POOL_KEY" \
    "--sv2-user=$PAYOUT.e2e" \
    "--sv2-expect-payout=$PAYOUT" \
    > "$LOGDIR/miner.log" 2>&1 &
MINER_PID=$!

echo "== waiting for a block"
HEIGHT="$BEFORE"
for _ in $(seq 1 120); do
    HEIGHT=$(rpc getblockcount)
    [ -n "$HEIGHT" ] && [ "$HEIGHT" -gt "$BEFORE" ] && break
    kill -0 "$MINER_PID" 2>/dev/null || fail "the miner exited; see $LOGDIR/miner.log"
    sleep 1
done
[ "$HEIGHT" -gt "$BEFORE" ] || fail "no block in 120s; see $LOGDIR/miner.log and $LOGDIR/pool.log"

HASH=$(rpc getblockhash "$HEIGHT")
echo "   height $HEIGHT, block $HASH"

echo
echo "== assertions"
rc=0

# 1. The node accepted it, and it is on the chain rather than merely known.
CONFIRMS=$(rpc getblock "$HASH" 1 | grep -o '"confirmations": *-\?[0-9]*' | grep -o -- '-\?[0-9]*$')
if [ "${CONFIRMS:-0}" -ge 1 ]; then
    echo "  ok   the node accepted the block ($CONFIRMS confirmation(s))"
else
    echo "  FAIL the block is not on the chain (confirmations ${CONFIRMS:-none})"
    rc=1
fi

# 2. It is the algorithm asked for, not the other one that happened to
#    validate. The pow type lives in nVersion bits 16-23, so a miner that
#    got this wrong would be mining against the other algorithm's limit.
POWTYPE=$(rpc getblock "$HASH" 1 | grep -o '"powtype": *"[^"]*"' | sed 's/.*"\([^"]*\)"$/\1/')
if [ "$POWTYPE" = "$ALGO" ]; then
    echo "  ok   getblock reports powtype $ALGO"
else
    echo "  FAIL getblock reports powtype '${POWTYPE:-none}', not $ALGO"
    rc=1
fi

# 3. The coinbase pays the address the miner asked for, read back out of the
#    block the node stored rather than out of anything the pool said.
COINBASE_TXID=$(rpc getblock "$HASH" 1 \
                | tr -d ' \n' | sed 's/.*"tx":\["\([0-9a-f]*\)".*/\1/')
RAW=$(rpc getrawtransaction "$COINBASE_TXID" 1 "$HASH")
if echo "$RAW" | grep -q "$PAYOUT"; then
    echo "  ok   the coinbase pays $PAYOUT"
else
    echo "  FAIL the coinbase does not pay $PAYOUT"
    echo "$RAW" | head -40
    rc=1
fi

# 4. The miner did the work. A block the node or the pool produced on its own
#    would pass the three above.
if grep -q "share accepted" "$LOGDIR/miner.log"; then
    echo "  ok   the miner's own log shows the pool accepting its share"
else
    echo "  FAIL the miner never saw a share accepted"
    tail -20 "$LOGDIR/miner.log"
    rc=1
fi

# 5. And it was on an extended channel, which is what lets the miner verify
#    its own payout rather than take the pool's word for the merkle root.
WANT_CHANNEL="${CHANNEL:-extended}"
if grep -q "$WANT_CHANNEL channel" "$LOGDIR/miner.log"; then
    echo "  ok   the channel was $WANT_CHANNEL, as asked for"
else
    echo "  FAIL the channel was not $WANT_CHANNEL"
    rc=1
fi
if [ "$WANT_CHANNEL" = "extended" ]; then
    if grep -q "coinbase pays $PAYOUT, verified" "$LOGDIR/miner.log"; then
        echo "  ok   the miner verified the payout from the coinbase it assembled"
    else
        echo "  FAIL the miner did not verify the payout itself"
        rc=1
    fi
elif grep -q "cannot verify the payout on a standard channel" "$LOGDIR/miner.log"; then
    # Saying so is the right answer here. A standard channel hands over a
    # finished merkle root, so there is nothing for the miner to read.
    echo "  ok   the miner says it cannot verify a payout on a standard channel"
else
    echo "  FAIL the miner did not say it cannot verify a standard channel's payout"
    rc=1
fi

# A restart of the pool, because this is the first real miner and the job
# tokens exist precisely so a nonce found before a reconnect is not applied to
# whatever job holds that id afterwards.
#   RECONNECT=1 avian/regtest-e2e.sh
if [ "${RECONNECT:-0}" = "1" ] && [ "$rc" -eq 0 ]; then
    echo
    echo "== restarting the pool under the miner"
    MARK=$(wc -l < "$LOGDIR/miner.log")
    AFTER_FIRST=$(rpc getblockcount)
    kill "$POOL_PID" 2>/dev/null
    wait "$POOL_PID" 2>/dev/null
    sleep 2
    "$SV2/target/release/avian-sv2-pool" \
        --datadir "$DATADIR" --chain regtest \
        --connect "127.0.0.1:$SV2_PORT" \
        --mining-bind "127.0.0.1:$MINING_PORT" \
        --pool-key "$DATADIR/pool-authority-key" \
        --coinbase-tag "Sv2/cpuminer-avian e2e//" \
        --algo "$ALGO" >> "$LOGDIR/pool.log" 2>&1 &
    POOL_PID=$!

    HEIGHT2="$AFTER_FIRST"
    for _ in $(seq 1 120); do
        HEIGHT2=$(rpc getblockcount)
        [ -n "$HEIGHT2" ] && [ "$HEIGHT2" -gt "$AFTER_FIRST" ] && break
        kill -0 "$MINER_PID" 2>/dev/null || break
        sleep 1
    done

    if ! kill -0 "$MINER_PID" 2>/dev/null; then
        echo "  FAIL the miner died when the pool went away"
        rc=1
    elif [ "$HEIGHT2" -gt "$AFTER_FIRST" ]; then
        echo "  ok   the miner reconnected and mined again (height $HEIGHT2)"
    else
        echo "  FAIL the miner never mined again after the pool came back"
        rc=1
    fi
    if tail -n +"$MARK" "$LOGDIR/miner.log" | grep -q "reconnected"; then
        echo "  ok   the miner logged the reconnection as a reconnection"
    else
        echo "  FAIL the miner did not log a reconnection"
        rc=1
    fi
fi

echo
if [ "$rc" -ne 0 ]; then
    echo "FAIL"
    echo "logs in $LOGDIR"
    exit 1
fi
echo "OK: aviand -> pool -> cpuminer-avian -> pool -> aviand, $ALGO, paid"

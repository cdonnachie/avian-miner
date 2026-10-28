#!/bin/bash
# The stage sweep itself, with no container around it. verify.sh runs this
# inside AMD's ROCm image; smoke.sh calls it directly, already being inside
# one. Run from the top of a built checkout.
#
# AVIAN_GPU_TRACE_STAGES=N stops after N kernels and compares d_hash against
# the CPU chain truncated to the same depth. One depth per run, not all at
# once, so covering the chain needs the sweep below. Depth 16 additionally
# needs AVIAN_GPU_NO_FINAL=1, or d_hash is a stage short and every sample
# differs for nothing; it is set throughout and is harmless shallower.
#
# --benchmark rather than a pool, because a forced order makes every share
# invalid by construction and those would otherwise reach someone's pool logs.
set -u
SECS=${SECS:-8}
ORDERS=${ORDERS:-"1F80675605446D40 542CB1E146A7D915"}
LOGS=${LOGS:-/tmp/avian-verify}
mkdir -p "$LOGS"
rc=0

for order in $ORDERS; do
    echo "=== order $order ==="
    tot=0
    bad=0
    first=""
    for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
        log=$LOGS/$order-$n.log
        AVIAN_GPU_FORCE_ORDER=$order \
        AVIAN_GPU_TRACE_STAGES=$n \
        AVIAN_GPU_NO_FINAL=1 \
        timeout "$SECS" ./avian-miner -a x16rt --gpu --benchmark > "$log" 2>&1 || true

        # "TRACE stage NN of ORDER:  D/T differ (stride ...)", count padded.
        read -r d t < <(awk '
            /TRACE stage/ && match($0, /: *[0-9]+\/[0-9]+ differ/) {
                split(substr($0, RSTART + 1), a, "/")
                d += a[1] + 0
                sub(/ differ.*/, "", a[2]); t += a[2] + 0
            }
            END { print d + 0, t + 0 }' "$log")

        tot=$((tot + ${t:-0}))
        bad=$((bad + ${d:-0}))
        if [ "${t:-0}" = 0 ]; then
            printf "  depth %2d: NO SAMPLES\n" "$n"
            rc=1
        elif [ "${d:-0}" != 0 ]; then
            printf "  depth %2d: %7d compared, %7d DIFFER\n" "$n" "$t" "$d"
            [ -z "$first" ] && first=$n
            rc=1
        fi
    done
    printf "  depths 1-16: %d comparisons, %d differing\n" "$tot" "$bad"
    [ -n "$first" ] && echo "  first divergence at depth $first"
done

echo "=== faults the runs themselves reported ==="
for pat in "setBlock_80" "launch failure" "memory access fault" "CUDA error"; do
    n=$(cat "$LOGS"/*.log 2>/dev/null | grep -ac "$pat")
    printf "  %-22s %s\n" "$pat" "$n"
    [ "$n" = 0 ] || rc=1
done

if [ "$rc" = 0 ]; then
    echo "=== PASS, every depth of every order agrees with the CPU ==="
else
    echo "=== NOT A PASS ==="
fi
exit $rc

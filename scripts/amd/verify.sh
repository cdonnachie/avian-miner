#!/bin/bash
# Find the first stage of the X16RT chain where gfx942 disagrees with the CPU.
#
# The same harness used against the 3090, which passed all sixteen depths of
# two forced orders. The difference now is the hardware, so whatever this
# finds is an AMD-specific fault rather than a mistake in the portable bodies.
#
# AVIAN_GPU_TRACE_STAGES=N stops after N kernels and compares d_hash against
# the CPU chain truncated to the same depth: one depth per run, not all at
# once. Depth 16 additionally needs AVIAN_GPU_NO_FINAL=1 or d_hash is a stage
# short and every sample differs for nothing; it is set throughout, harmless
# at shallower depths.
#
# --benchmark rather than a pool: a forced order makes every share invalid by
# construction, and those would otherwise land in someone's pool logs.
set -u
SRC=${SRC:-$HOME/src/avian-miner}
R=$(getent group render | cut -d: -f3)
V=$(getent group video | cut -d: -f3)
SECS=${SECS:-8}
ORDERS=${ORDERS:-"1F80675605446D40 542CB1E146A7D915"}

docker run --rm \
    --device=/dev/kfd --device=/dev/dri --group-add "$R" --group-add "$V" \
    --ipc=host --shm-size 8G \
    -e SECS="$SECS" -e ORDERS="$ORDERS" \
    -v "$SRC":/src -w /src \
    rocm/dev-ubuntu-24.04:latest \
    bash -c '
set -u
mkdir -p /src/verifylogs
rc=0
for order in $ORDERS; do
    echo "=== order $order ==="
    first=""
    for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
        log=/src/verifylogs/$order-$n.log
        AVIAN_GPU_FORCE_ORDER=$order \
        AVIAN_GPU_TRACE_STAGES=$n \
        AVIAN_GPU_NO_FINAL=1 \
        timeout "$SECS" ./avian-miner -a x16rt --gpu --benchmark > "$log" 2>&1 || true

        # "TRACE stage NN of ORDER:  D/T differ (stride ...)", the count padded.
        read -r d t < <(awk "
            /TRACE stage/ && match(\$0, /: *[0-9]+\/[0-9]+ differ/) {
                split(substr(\$0, RSTART+1), a, \"/\")
                d += a[1] + 0
                sub(/ differ.*/, \"\", a[2]); t += a[2] + 0
            }
            END { print d + 0, t + 0 }" "$log")

        if [ "$t" = 0 ]; then
            printf "  depth %2d: NO SAMPLES\n" "$n"
            rc=1
        elif [ "$d" != 0 ]; then
            printf "  depth %2d: %7d compared, %7d DIFFER\n" "$n" "$t" "$d"
            [ -z "$first" ] && first=$n
            rc=1
        else
            printf "  depth %2d: %7d compared, agrees\n" "$n" "$t"
        fi
    done
    if [ -n "$first" ]; then
        echo "  first divergence at depth $first"
        # Which algorithm that depth is, read from the order itself: the
        # order is 16 hex nibbles, leftmost first.
        echo "  that stage is nibble $first of $order"
    fi
done

echo "=== faults the runs reported ==="
for pat in "setBlock_80" "launch failure" "memory access fault" "CUDA error"; do
    n=$(cat /src/verifylogs/*.log 2>/dev/null | grep -ac "$pat")
    printf "  %-22s %s\n" "$pat" "$n"
done
exit $rc
'

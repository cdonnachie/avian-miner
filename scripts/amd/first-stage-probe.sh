#!/bin/bash
# Runs inside the container. One uniform order per algorithm, traced at depth
# 1 only, so what is measured is that algorithm's 80-byte kernel.
set -u
SECS=${SECS:-6}
NAMES=(blake bmw groestl jh keccak skein luffa cubehash \
       shavite simd echo hamsi fugue shabal whirlpool sha512)
NIB=(0 1 2 3 4 5 6 7 8 9 A B C D E F)

bad=0
for i in $(seq 0 15); do
    n=${NIB[$i]}
    ord=""
    for _ in $(seq 1 16); do ord="$ord$n"; done

    AVIAN_GPU_FORCE_ORDER="$ord" \
    AVIAN_GPU_TRACE_STAGES=1 \
    AVIAN_GPU_NO_FINAL=1 \
    timeout "$SECS" ./avian-miner -a x16rt --gpu --benchmark > /tmp/s.log 2>&1 || true

    # "TRACE stage  1 of ORDER:  D/T differ (stride ...)"
    read -r d t < <(awk '
        /TRACE stage/ && match($0, /: *[0-9]+\/[0-9]+ differ/) {
            split(substr($0, RSTART + 1), a, "/")
            d += a[1] + 0
            sub(/ differ.*/, "", a[2]); t += a[2] + 0
        }
        END { print d + 0, t + 0 }' /tmp/s.log)

    if [ "${t:-0}" = 0 ]; then
        printf "  %s %-10s NO SAMPLES\n" "$n" "${NAMES[$i]}"
        bad=1
    elif [ "${d:-0}" != 0 ]; then
        printf "  %s %-10s DIFFERS  (%s of %s)\n" "$n" "${NAMES[$i]}" "$d" "$t"
        bad=1
    else
        printf "  %s %-10s ok       (%s compared)\n" "$n" "${NAMES[$i]}" "$t"
    fi
done
exit $bad

#!/usr/bin/env bash
# Report avian-miner's rate to HiveOS, once a cycle.
#
# Sourced, not run, so it sets khs and stats and never exits: an exit here
# would take the agent's whole stats pass with it.
#
# The source is the miner's own API, the cpuminer one, over loopback. Two
# commands: "threads" gives a line per mining thread, which for a GPU run is
# a line per card, and "summary" gives the totals. Both answers end in a NUL
# that nc passes through, hence the tr.

khs=0
stats=

avian_api_port=${CUSTOM_API_PORT:-4048}
avian_timeout=${API_TIMEOUT:-10}

avian_ask() {
    echo "$1" | nc -w "$avian_timeout" 127.0.0.1 "$avian_api_port" 2>/dev/null \
        | tr -d '\000'
}

avian_summary=$(avian_ask summary)
avian_threads=$(avian_ask threads)

# No answer means the miner is starting, or has died and HiveOS is about to
# notice by other means. Reporting zero would be a lie either way, so leave
# khs and stats as they are and say nothing.
if [[ -n $avian_summary && -n $avian_threads ]]; then

    avian_field() {
        [[ $avian_summary =~ (^|\;)$1=([^;|]*) ]] && echo "${BASH_REMATCH[2]}"
    }

    avian_ver=$(avian_field VER)
    avian_algo=$(avian_field ALGO)
    avian_acc=$(avian_field ACC)
    avian_rej=$(avian_field REJ)
    avian_uptime=$(avian_field UPTIME)

    # Each thread reports its own rate with its own scale prefix, so the
    # prefix decides the multiplier rather than being assumed. A GPU thread
    # also reports the PCIe bus of the card it is on, which is how HiveOS
    # pairs a rate with a row of its GPU table; it is absent, not -1, when
    # the backend could not tell.
    avian_hs=()
    avian_bus=()
    avian_total=0
    while IFS= read -r avian_part; do
        [[ $avian_part =~ \;([kMGT]?)H/s=([0-9.]+) ]] || continue
        case ${BASH_REMATCH[1]} in
            k) avian_mul=1000 ;;
            M) avian_mul=1000000 ;;
            G) avian_mul=1000000000 ;;
            T) avian_mul=1000000000000 ;;
            *) avian_mul=1 ;;
        esac
        avian_one=$(awk -v v="${BASH_REMATCH[2]}" -v m="$avian_mul" \
                        'BEGIN { printf "%.0f", v * m }')
        avian_hs+=("$avian_one")
        avian_total=$(( avian_total + avian_one ))

        [[ $avian_part =~ \;BUS=([0-9]+) ]] && avian_bus+=("${BASH_REMATCH[1]}")
    done < <(tr '|' '\n' <<< "$avian_threads")

    if (( ${#avian_hs[@]} > 0 )); then
        khs=$(awk -v t="$avian_total" 'BEGIN { printf "%.3f", t / 1000 }')

        avian_hs_json=$(printf '%s\n' "${avian_hs[@]}" | jq -cs '.')

        # Only when every thread answered, because a partial array would
        # pair rates with the wrong cards, which is worse than none at all.
        if (( ${#avian_bus[@]} == ${#avian_hs[@]} )); then
            avian_bus_json=$(printf '%s\n' "${avian_bus[@]}" | jq -cs '.')
        else
            avian_bus_json='[]'
        fi

        # hs_units is stated rather than left to the default, because the
        # miner's API reports kH/s and this array is H/s; a wrong guess here
        # is a thousandfold error on the dashboard and nowhere else.
        #
        # No temp or fan for a GPU run. Those belong to the cards, and the
        # HiveOS agent reads them from the driver, which is a better source
        # than a miner that only knows what it was told at startup. A CPU run
        # reports its one temperature, repeated to the length of hs, because
        # HiveOS pairs the two arrays by index rather than broadcasting.
        avian_gpu=$(jq -r '.gpu // false' "$CUSTOM_CONFIG_FILENAME" 2>/dev/null)

        if [[ $avian_gpu == true ]]; then
            stats=$(jq -nc \
                --argjson hs "$avian_hs_json" \
                --argjson bus "$avian_bus_json" \
                --arg algo "$avian_algo" --arg ver "$avian_ver" \
                --arg uptime "$avian_uptime" \
                --arg ac "$avian_acc" --arg rj "$avian_rej" \
                '{$hs, hs_units: "hs", $algo, $ver, $uptime, ar: [$ac, $rj]}
                 + (if ($bus | length) > 0 then {bus_numbers: $bus} else {} end)')
        else
            avian_temp=$(cpu-temp 2>/dev/null)
            [[ -z $avian_temp ]] && avian_temp=0
            avian_temps=()
            for (( avian_i = 0; avian_i < ${#avian_hs[@]}; avian_i++ )); do
                avian_temps+=("$avian_temp")
            done
            stats=$(jq -nc \
                --argjson hs "$avian_hs_json" \
                --argjson temp "$(printf '%s\n' "${avian_temps[@]}" | jq -cs '.')" \
                --arg algo "$avian_algo" --arg ver "$avian_ver" \
                --arg uptime "$avian_uptime" \
                --arg ac "$avian_acc" --arg rj "$avian_rej" \
                '{$hs, hs_units: "hs", $temp, $algo, $ver, $uptime,
                  ar: [$ac, $rj]}')
        fi
    fi
fi

unset avian_api_port avian_timeout avian_summary avian_threads avian_ver
unset avian_algo avian_acc avian_rej avian_uptime avian_hs avian_total
unset avian_part avian_mul avian_one avian_gpu avian_temp avian_temps
unset avian_i avian_bus avian_hs_json avian_bus_json
unset -f avian_ask avian_field

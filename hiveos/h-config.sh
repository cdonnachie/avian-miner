#!/usr/bin/env bash
# Turn the HiveOS flight sheet into avian-miner's JSON config.
#
# The miner takes -c FILE holding one JSON object whose keys are long option
# names, so this writes that rather than a command line: the "Setup Miner
# Config" box then shows the user exactly what the miner will be given, and
# an extra option is one more line rather than a quoting problem.

cd "$(dirname "$0")" || exit 1
. h-manifest.conf

[[ -z $CUSTOM_CONFIG_FILENAME ]] && echo "no CUSTOM_CONFIG_FILENAME" && exit 1
mkdir -p "$(dirname "$CUSTOM_CONFIG_FILENAME")"

conf=$(jq --null-input \
          --arg bind "127.0.0.1:${CUSTOM_API_PORT:-4048}" \
          '{"api-bind": $bind}')

[[ -n $CUSTOM_TEMPLATE ]] &&
    conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_TEMPLATE" \
              '$c + {user: $v}')
[[ -n $CUSTOM_URL ]] &&
    conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_URL" \
              '$c + {url: $v}')
[[ -n $CUSTOM_PASS ]] &&
    conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_PASS" \
              '$c + {pass: $v}')
[[ -n $CUSTOM_ALGO ]] &&
    conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_ALGO" \
              '$c + {algo: $v}')

# x16rt is the GPU algorithm. It has a CPU implementation and will quietly
# use it, at a thousandth of the rate, which on a rig looks like the cards
# have failed rather than like a missing flag. Default it on and say so; the
# user config below is merged afterwards, so "gpu": false still wins.
if [[ $CUSTOM_ALGO == x16rt ]]; then
    conf=$(jq --null-input --argjson c "$conf" '$c + {gpu: true}')
    echo "avian-miner: x16rt, so mining on the GPU. Put \"gpu\": false in the"
    echo "             extra config to run it on the CPU instead."
fi

# One JSON fragment a line, recursively merged, so a later line overrides an
# earlier key. Same shape as HiveOS's own cpuminer-opt integration, which is
# what anyone arriving here will have used before.
if [[ -n $CUSTOM_USER_CONFIG ]]; then
    while read -r line; do
        [[ -z ${line// } ]] && continue
        merged=$(jq -s '.[0] * .[1]' <<< "$conf {$line}" 2>/dev/null)
        if [[ -z $merged ]]; then
            echo "avian-miner: ignoring extra config that is not JSON: $line"
            continue
        fi
        conf=$merged
    done <<< "$CUSTOM_USER_CONFIG"
fi

echo "$conf" | jq . > "$CUSTOM_CONFIG_FILENAME"

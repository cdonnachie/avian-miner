#!/usr/bin/env bash
# Turn the HiveOS flight sheet into avian-miner's JSON config.
#
# The miner takes -c FILE holding one JSON object whose keys are long option
# names, so this writes that rather than a command line: the "Setup Miner
# Config" box then shows the user exactly what the miner will be given, and
# an extra option is one more line rather than a quoting problem.

# ${BASH_SOURCE[0]} and not $0, which is not a matter of taste: HiveOS
# sources this script rather than running it, the same way it sources
# h-stats.sh, so $0 is the agent's own name and dirname $0 is wherever the
# agent happens to be. The cd then succeeds against the wrong directory and
# the source below fails in a directory that does contain the file. Seen on a
# rig as "./h-manifest.conf: No such file or directory" with h-manifest.conf
# sitting beside this script. BASH_SOURCE is the script's own path either way.
#
# It went unnoticed because nothing here needs the manifest to be read:
# HiveOS exports CUSTOM_CONFIG_FILENAME itself, and CUSTOM_API_PORT falls back
# to the same 4048 both here and in h-stats.sh. So the two agreed by
# coincidence, and any value edited into the manifest was silently ignored.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
# ./ matters: the dot command searches PATH, not the working directory, so a
# bare name fails here however carefully the cd above was done.
. ./h-manifest.conf

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
            # Said once, with the shape, because the thing anyone will type
            # here first is the command-line flag they already know.
            echo "             This box takes JSON, one fragment a line, so"
            echo "             --gpu is \"gpu\": true and --devices 1 is"
            echo "             \"devices\": \"1\"."
            continue
        fi
        conf=$merged
    done <<< "$CUSTOM_USER_CONFIG"
fi

echo "$conf" | jq . > "$CUSTOM_CONFIG_FILENAME"

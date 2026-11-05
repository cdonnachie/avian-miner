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

# The extra config is collected before the flight-sheet fields are turned
# into keys, rather than after, because one of its keys decides what the
# others mean: "sv2" selects Stratum v2, where the sheet's URL, wallet and
# pass fields belong to different options entirely. It is still merged last,
# so it still overrides everything.
#
# One JSON fragment a line, recursively merged, so a later line overrides an
# earlier key. Same shape as HiveOS's own cpuminer-opt integration, which is
# what anyone arriving here will have used before.
extra='{}'
if [[ -n $CUSTOM_USER_CONFIG ]]; then
    while read -r line; do
        [[ -z ${line// } ]] && continue
        merged=$(jq -s '.[0] * .[1]' <<< "$extra {$line}" 2>/dev/null)
        if [[ -z $merged ]]; then
            echo "avian-miner: ignoring extra config that is not JSON: $line"
            # Said once, with the shape, because the thing anyone will type
            # here first is the command-line flag they already know.
            echo "             This box takes JSON, one fragment a line, so"
            echo "             --gpu is \"gpu\": true and --devices 1 is"
            echo "             \"devices\": \"1\"."
            continue
        fi
        extra=$merged
    done <<< "$CUSTOM_USER_CONFIG"
fi

# Which protocol this sheet is for. Two ways to say Stratum v2, because a
# sheet can only express it in one of two places: a scheme on the Pool URL,
# or "sv2" written into the extra config directly. A bare host:port cannot be
# the signal, since that is also how people type a Stratum v1 pool.
#
# The library takes host:port and resolves it itself, so a scheme is stripped
# here rather than passed on.
sv2_pool=$(jq -r '.sv2 // empty' <<< "$extra")
if [[ -z $sv2_pool ]]; then
    case $CUSTOM_URL in
        sv2://*|stratum2://*|stratum2+tcp://*)
            sv2_pool=${CUSTOM_URL#*://}
            sv2_pool=${sv2_pool%%/*}
            ;;
    esac
fi

conf=$(jq --null-input \
          --arg bind "127.0.0.1:${CUSTOM_API_PORT:-4048}" \
          '{"api-bind": $bind}')

if [[ -n $sv2_pool ]]; then
    # Stratum v2 takes its own options and ignores url, user and pass, so
    # those are left out rather than written beside the ones that are read.
    # The config box is what an operator checks when a rig will not mine, and
    # a pool address in it that the miner is not using is worse than nothing.
    conf=$(jq --null-input --argjson c "$conf" --arg v "$sv2_pool" \
              '$c + {sv2: $v}')
    [[ -n $CUSTOM_TEMPLATE ]] &&
        conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_TEMPLATE" \
                  '$c + {"sv2-user": $v}')
    # Stratum v2 has no password, and the sheet has nowhere else to put the
    # pool's authority key, so the Pass field is read as one. Shape-checked
    # rather than passed on: the miner exits when a key does not parse, and a
    # sheet left with the usual "x" would otherwise put the rig in a restart
    # loop. A key is 64 hex characters or about 52 of base58.
    if [[ $CUSTOM_PASS =~ ^[0-9a-fA-F]{64}$ ]] ||
       [[ ${#CUSTOM_PASS} -ge 40 && $CUSTOM_PASS =~ ^[1-9A-HJ-NP-Za-km-z]+$ ]]; then
        conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_PASS" \
                  '$c + {"sv2-authority": $v}')
    elif [[ -n $CUSTOM_PASS ]]; then
        echo "avian-miner: the Pass field is not an authority key, so it is"
        echo "             ignored. Stratum v2 has no password, so that field"
        echo "             is where the key the pool publishes goes."
    fi
    # Said only when the two disagree, which is the case where someone moved
    # a working sheet to Stratum v2 by editing the extra config and left the
    # Pool URL box as it was.
    if [[ -n $CUSTOM_URL && $sv2_pool != "${CUSTOM_URL#*://}" ]]; then
        echo "avian-miner: the Pool URL field is unused. \"sv2\" in the extra"
        echo "             config is what sets the pool."
    fi
else
    [[ -n $CUSTOM_TEMPLATE ]] &&
        conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_TEMPLATE" \
                  '$c + {user: $v}')
    [[ -n $CUSTOM_URL ]] &&
        conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_URL" \
                  '$c + {url: $v}')
    [[ -n $CUSTOM_PASS ]] &&
        conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_PASS" \
                  '$c + {pass: $v}')
fi

[[ -n $CUSTOM_ALGO ]] &&
    conf=$(jq --null-input --argjson c "$conf" --arg v "$CUSTOM_ALGO" \
              '$c + {algo: $v}')

# x16rt is the GPU algorithm. It has a CPU implementation and will quietly
# use it, at a thousandth of the rate, which on a rig looks like the cards
# have failed rather than like a missing flag. Default it on and say so; the
# extra config is merged afterwards, so "gpu": false still wins.
if [[ $CUSTOM_ALGO == x16rt ]]; then
    conf=$(jq --null-input --argjson c "$conf" '$c + {gpu: true}')
    echo "avian-miner: x16rt, so mining on the GPU. Put \"gpu\": false in the"
    echo "             extra config to run it on the CPU instead."
fi

conf=$(jq -s '.[0] * .[1]' <<< "$conf $extra")

echo "$conf" | jq . > "$CUSTOM_CONFIG_FILENAME"

# Read back from the file rather than from $conf, so this describes what the
# miner will actually be given, extra config and all.
if [[ -n $(jq -r '.sv2 // empty' "$CUSTOM_CONFIG_FILENAME") ]]; then
    echo "avian-miner: Stratum v2, connecting to $(jq -r .sv2 "$CUSTOM_CONFIG_FILENAME")"
    if [[ -z $(jq -r '.["sv2-authority"] // empty' "$CUSTOM_CONFIG_FILENAME") ]]; then
        echo "             No authority key, so the connection is encrypted but"
        echo "             the pool is not authenticated and anything that can"
        echo "             reroute the traffic can take the hashrate. Put the"
        echo "             key the pool publishes in the Pass field."
    fi
fi

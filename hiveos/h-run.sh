#!/usr/bin/env bash
# Start avian-miner under HiveOS.
#
# Foreground and exec'd, because HiveOS runs this inside a screen session and
# takes the exit status as the miner's: a wrapper process in between turns a
# crash into a clean exit and the rig never restarts it.

cd "$(dirname "$0")" || exit 1
. h-manifest.conf

if [[ ! -f $CUSTOM_CONFIG_FILENAME ]]; then
    echo "avian-miner: no config at $CUSTOM_CONFIG_FILENAME."
    echo "             Apply the flight sheet again so h-config.sh writes it."
    exit 1
fi

# The GPU backends are dlopened from beside the binary, which is this
# directory, so nothing needs to be on LD_LIBRARY_PATH.
exec ./avian-miner --config="$CUSTOM_CONFIG_FILENAME"

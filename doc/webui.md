# The web dashboard

`avian-webui` is a small read-only web dashboard for a running miner. It is a
sidecar: a separate process that polls the miner's own API over loopback and
serves a page the rest of the network can open. The miner is not an HTTP
server and does not become one — a process whose job is to hash should not
also be parsing requests from the LAN.

It shows the hash rate and its trend, a row per card with that card's own
rate and the blocks it has found, and the recent solves with the card that
found each one.

    ./avian-webui          # Linux, HiveOS
    avian-webui.exe        # Windows

That serves `http://<this machine>:4049/` and reads the miner on
`127.0.0.1:4048`. Both ends are configurable; `--help` lists everything.

It ships in every release beside `avian-miner`, needs no runtime installed
and no files beside it: the page is compiled into the binary, so there is one
file and it either runs or is not there. On Windows it is code-signed with
the same certificate as the miner, which is the main reason it is a C program
rather than the Python script it began as — the one file in a release that
opens a listening socket should not also be the one nobody can verify.

It is a separate process, started separately, and **there is no miner flag
that turns it on**. The miner does not launch it, supervise it or know it
exists; the only miner-side setting involved is `--api-bind`, below, and that
is an existing option rather than one this dashboard added. Start it, stop it
or restart it without touching the miner, and if it dies the miner does not
notice — which is the point of keeping the hashing process out of it.

`webui/avian-webui.py` in the source tree is the same dashboard as a Python
script, serving the same page and the same JSON. It is kept for the moment as
a second opinion while the C one proves itself on real rigs, and is not part
of any release.

## Turning the miner's API on first

The dashboard has nothing to read until the miner opens its API, and the
miner does not open it unless asked. This is the usual reason the page comes
up saying the miner is unreachable.

    avian-miner --api-bind 127.0.0.1:4048 ...

or, in the config file:

    "api-bind": "127.0.0.1:4048"

**On HiveOS this is already done.** `h-config.sh` writes `api-bind` into the
generated config from `CUSTOM_API_PORT`, so a rig installed as a custom miner
is already listening on 4048. Nothing sets it anywhere else, which is why the
same miner on Windows or a plain Linux box has nothing on that port until you
add the flag yourself.

**The two ends have to agree on the port.** The dashboard polls
`127.0.0.1:4048` unless told otherwise, so a miner put somewhere else needs
the dashboard told the same:

    avian-miner --api-bind 127.0.0.1:14048 ...
    ./avian-webui --miner 127.0.0.1:14048

Neither process waits for the other, and the order they start in does not
matter. The dashboard serves its page whether or not the miner is up, names
the address it got no answer from — which is what a port mismatch looks like
— and fills in within one poll of the miner appearing. Nothing has to be
restarted to connect them.

Better than keeping two flags in step, if the miner is started from a config
file: point the dashboard at that same file and let it read the port out of
`api-bind` itself.

    ./avian-webui \
        --miner-config /hive/miners/custom/avian-miner/avian-miner.conf

That is the HiveOS path, where the file is the one `h-config.sh` generates.
`--miner-config` takes precedence over `--miner`, understands the same three
forms of `api-bind` the miner does (`ip:port`, an ip alone, a bare port) and
reads loopback for a wildcard bind, since `0.0.0.0` is not an address you can
connect to. It re-reads on change, so applying a flight sheet that moves the
API port moves the dashboard with it; and when the file is missing, is not
JSON, or has no `api-bind` at all, the page says which of those it is rather
than reporting a miner that is not answering.

Leave the miner's own API on `127.0.0.1`. It is the dashboard that is meant
to be reachable, and the two are not the same thing: the miner's API has a
`seturl` and a `quit` behind `--api-remote`, and the dashboard has no way to
reach either.

## Where to bind it

The default is `0.0.0.0:4049` — every interface, so another machine can open
it. That is the point of the sidecar, and it is also the thing to think about
before leaving it running:

    ./avian-webui --bind 0.0.0.0:4049      # the default
    ./avian-webui --bind 127.0.0.1:4049    # this machine only
    ./avian-webui --bind 192.168.1.50:4049 # one interface

There is no password on it. Adding one would invite trusting it, and a
dashboard is the wrong place to learn that a home-made auth check was weak.
Bind it where you would be comfortable publishing the numbers: a home or rig
LAN, not a public address and not a port forwarded through a router.

What it exposes, if someone does reach it: the pool address, the algorithm,
hash rates, share counts, and the heights of blocks you found. No keys, no
wallet address, no control of any kind.

## Read only

Three separate things make that true, because one would be a claim rather
than a property:

* The only commands it will send the miner are `summary`, `threads` and
  `blocks`, fixed in `miner_commands`. Nothing from an HTTP request reaches
  the miner socket, so there is no path to `seturl` or `quit` even if someone
  asks for one.
* Only `GET` and `HEAD` are answered. Everything else gets `405` with an
  `Allow: GET, HEAD`.
* Nothing is written to disk. The history lives in memory and goes when the
  process does.

## Which card found the block

The miner keeps the last sixteen solves with the thread that found each one,
and on a GPU run a thread is a card. `solved_blocks_recent` in `cpu-miner.c`
hands them to the API's `blocks` command; the running per-card tally is `SOL`
in each `threads` record. The totals come from `solved_block_count` as
before, so a long run that has overflowed the ring still reports every block
it found — only the details of the older ones are gone.

A solve can arrive without a card against it. The miner credits one only when
it knows which thread submitted the share, and the stats queue it reads that
from is eight deep; a burst can overflow it. The dashboard shows those as
*unrecorded* rather than guessing, and the per-card tallies leave them out,
so on a rig that has lost one the columns will not sum to the total. That is
the honest arrangement: a wrong card is worse than no card, because the whole
point of the column is deciding which slot is earning.

## What it reads

The dashboard is a client of the miner's text API and adds nothing the API
does not already report, so anything here can be scripted without it:

| Command | Field | Meaning |
|---|---|---|
| `summary` | `HS`, `KHS` | total hash rate, H/s and kH/s |
| | `ACC`, `REJ`, `STALE` | accepted, rejected and stale shares |
| | `SOL` | blocks solved this run |
| | `ALGO`, `VER`, `URL`, `UPTIME` | what is running, where, and for how long |
| | `DIFF`, `ACCMN` | network difficulty, accepted shares per minute |
| `threads` | `CPU`, `<prefix>H/s` | thread number and its rate, prefix included |
| | `GPU`, `CARD`, `VENDOR`, `BUS` | the card, when mining on one |
| | `SOL` | blocks this card has found |
| `blocks` | `NUM`, `TS`, `HEIGHT` | which solve, when, and at what height |
| | `CPU`, `GPU`, `CARD`, `BUS` | the card that found it, absent if unknown |
| | `DIFF`, `NETDIFF` | the share's difficulty and the network's |

Any of them answers on a bare TCP connection, one command per connection:

    echo summary | nc 127.0.0.1 4048

## Limits

The history is in memory only, so restarting the sidecar starts the graph
over; `--history` sets how many samples are kept, 720 by default, which is an
hour at the default five second interval. The miner keeps the last sixteen
solves and no more. There is no database and no log, by design — this is a
window onto a running miner, not a record of one.

No per-card temperature, fan or power, because the miner does not have them:
the backends report identity and nothing else. On HiveOS the agent reads
those from the driver, which is a better source than a miner would be.

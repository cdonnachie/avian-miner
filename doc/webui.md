# The web dashboard

`avian-webui` is a small read-only web dashboard for a running miner. It is a
sidecar: a separate process that polls the miner's own API over loopback and
serves a page the rest of the network can open.

It shows the hash rate and its trend, a row per card with that card's own
rate and the blocks it has found, and the recent solves with the card that
found each one.

    ./avian-webui          # Linux, HiveOS
    avian-webui.exe        # Windows

That serves `http://<this machine>:4049/` and reads the miner on
`127.0.0.1:4048`. Both ends are configurable; `--help` lists everything.

Or let the miner run it, which is usually easier:

    avian-miner --webui ...

See [Letting the miner run it](#letting-the-miner-run-it) below.

It ships in every release beside `avian-miner`, needs no runtime installed
and no files beside it: the page is compiled into the binary, so there is one
file and it either runs or is not there. On Windows it is code-signed with
the same certificate as the miner, which is the main reason it is a C program
rather than the Python script it began as — the one file in a release that
opens a listening socket should not also be the one nobody can verify.

## Why a separate process

Not because a miner cannot serve HTTP. It already opens a listening socket
and parses requests on it, so serving a page would be a difference of degree
rather than of kind. The reason is what the two sockets are *for*. The
miner's API is a loopback control socket with `seturl` and `quit` on it
behind `--api-remote`; the dashboard is meant to be reachable from the LAN.
Keeping them in separate address spaces means a parsing bug in the half that
faces a network costs a dashboard rather than hours of hashing — and that
stays a property of the arrangement rather than a rule somebody has to keep
remembering.

Two useful things fall out of it. The dashboard can be restarted without
touching the miner, which matters more than it sounds: restarting the miner
costs a re-benchmark and a share target renegotiated with the pool. And one
dashboard can watch several miners, which is the normal case on a machine
mining both algorithms at once.

What the arrangement was missing was somebody to start the second process,
which is what `--webui` is for.

## Letting the miner run it

`--webui` runs the dashboard as a child process and keeps it running:

    avian-miner --webui ...                  # 0.0.0.0:4049, the default
    avian-miner --webui=127.0.0.1:4049 ...   # this machine only

The miner finds `avian-webui` beside its own binary, the same way it finds
the GPU backends, so a release directory copied somewhere else still works
and a stray `avian-webui` elsewhere on the system is never the one that runs.
It hands over its own API address, so the two ends cannot disagree about a
port.

If the dashboard stops, it is started again, with a doubling backoff to
thirty seconds — so the failure that repeats, a port already taken, names
itself in the log instead of spinning. It does not outlive the miner either
way the miner goes away: a job object on Windows, `PR_SET_PDEATHSIG` on
Linux.

`--webui` also turns the miner's own API on, on loopback, if it is not on
already, since a dashboard with nothing to read can only report the miner as
unreachable. It never implies `--api-remote`.

In a config file, which is how HiveOS passes options:

    "webui": true                  # the default address
    "webui": "127.0.0.1:4049"      # or a particular one

On HiveOS that goes in the extra config box and needs no change to the `h-`
scripts: the miner stays the one process HiveOS supervises, and it
supervises the dashboard itself. Started this way or by hand it is the same
program, with the same options and the same read-only guarantees.

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
| | `CPUS`, `CPU`, `LCPUS`, `CORES`, `CPUSET`, `FEATURES` | the machine: threads running, what the CPU calls itself, its logical CPUs and cores, the CPUs this miner is kept to, and the instruction sets it has that the hashing can use; `CORES` only when how the CPUs pair could be read, `CPUSET` only when `--cpu-affinity` confines it |
| | `DIFF`, `ACCMN` | network difficulty, accepted shares per minute |
| | `TEMP`, `FREQ` | CPU package temperature and mean effective clock in MHz; both 0 when unknown, as they were before there were sensors |
| | `POWER`, `SENSORS`, `TEMP_<die>` | package watts, what answered (`k10temp`, `LibreHardwareMonitor`...), and one temperature per die (`TEMP_CCD1`); each only when known |
| `threads` | `CPU`, `<prefix>H/s` | thread number and its rate, prefix included |
| | `BUSY` | the share of its CPU the thread got since the last call, in percent |
| | `PIN`, `CLOCK` | on a CPU run with an affinity mask: the logical CPU the thread is pinned to and that CPU's effective MHz |
| | `GPU`, `CARD`, `VENDOR`, `BUS` | the card, when mining on one |
| | `TEMP`, `MEMTEMP`, `HOTSPOT`, `FAN`, `POWER`, `CLOCK`, `MEMCLOCK` | the card's readings: degrees, percent, watts and MHz, each only when its driver reports it |
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

The readings have gaps, each for a reason given in the next section: no AMD
card readings on Windows, no memory temperature from consumer Nvidia boards,
and no CPU temperature or power on Windows without the `avian-sensors`
service or LibreHardwareMonitor beside the miner. Where a reading is missing
the page leaves the column out rather than showing a zero, and says
underneath what it looked for.

## Temperatures, power and clocks

The miner reads these itself and puts them on the API, so the dashboard
shows them from wherever it runs, and so can anything else that reads the
API. Where they come from:

* **Nvidia cards**: NVML, the driver's own monitoring library, loaded at run
  time if it is installed, so the build needs no SDK and a machine without
  the driver loses the readings and nothing else. Core temperature, fan,
  board power, core and memory clock. The memory junction temperature that
  tools like HWiNFO show on GDDR6X boards is not public in NVML; the field
  that carries it answers on datacenter boards, and the miner reports it
  when it does.
* **AMD cards, Linux only**: the hwmon node the amdgpu driver puts under the
  card's PCI device, found by the same bus number the API already reports.
  Edge, junction and memory temperatures, fan, power, and the current DPM
  level for both clocks. There is no ADL, so an AMD card on Windows reports
  identity and rate alone.
* **CPU on Linux**: the `k10temp`, `zenpower` or `coretemp` driver, found by
  name rather than by hwmon number, which changes between boots. On Zen
  that gives `Tctl` or `Tdie` for the package and one `Tccd` per die, which
  arrive as `TEMP_CCD1`, `TEMP_CCD2`. Package power from RAPL when it is
  readable, which on most kernels since 5.10 means as root — a rig, not a
  desktop. Per-CPU clocks from cpufreq.
* **CPU on Windows**: the effective clock of each logical CPU from the
  `Processor Information` performance counters, which is the kernel's own
  APERF/MPERF ratio and includes boost. Temperature and power need help,
  because Windows gives user mode no way to read either: every tool that
  shows them — HWMonitor, HWiNFO, LibreHardwareMonitor — ships a kernel
  driver to read the MSRs and SMN registers, and a miner carrying a ring-0
  driver is a different kind of program, exactly the one antivirus is
  right to flag. So the miner reads them from one of two neighbours, tried
  in this order, each picked up within half a minute of appearing:
  the **`avian-sensors` service** from this zip, described next, or
  **LibreHardwareMonitor** with Options > Remote Web Server > Run switched
  on, read from the JSON its web server publishes on port 8085. (Its WMI
  namespace, which older tools read, went in 0.9.5.) On some AMD platforms
  Windows reports the same clock for every core on a CCD.

### The avian-sensors service

`avian-sensors.exe` is a small Windows service that reads the CPU's package
temperature, the per-CCD temperatures on the AMD parts that have them, and
package power, once a second, and hands the latest reading to whatever
opens its named pipe. The miner opens it as an ordinary file, so the miner
itself is never elevated; only the service is, and only to read. It is
optional, and a miner without it mines exactly as before.

It reads the registers through **PawnIO** (<https://pawnio.eu>), the signed
driver LibreHardwareMonitor is built on, which runs small signed modules
that each know one family of hardware. Nothing in this package is a driver.
To set it up:

1. Install PawnIO from its site, if LibreHardwareMonitor has not already
   done so.
2. Have the two module files beside `avian-sensors.exe`: `AMDFamily17.bin`
   and `IntelMSR.bin`, the signed builds published at
   <https://github.com/namazso/PawnIO.Modules/releases> (LGPL). A release
   built normally already carries them, fetched and checksummed by
   `scripts/windows/fetch-pawnio-modules.sh`; if the installer says they
   are missing, copy them in from that release's zip and the service picks
   them up within thirty seconds. The driver refuses an altered one, so use
   them as published.
3. Tick "Install the CPU temperature and power service" in the installer,
   or from an administrator prompt in the miner's folder run
   `avian-sensors --install`. Either way it starts at once and at every
   boot; the uninstaller removes it, as does `avian-sensors --uninstall`.

The miner's log then says `the CPU reports through the avian-sensors
service (pawnio/amd)` and the dashboard's CPU tiles fill in. If something is
missing the service says what in `%ProgramData%\avian-miner\avian-sensors.log`,
serves "unknown", and tries again every thirty seconds; `avian-sensors
--console` runs it in a window instead, for a look, and `--fake` with either
invents readings so the pipe can be tested without PawnIO.

What it decodes comes from LibreHardwareMonitor's `Amd17Cpu.cs` and
`IntelCpu.cs`, which follow the Linux `k10temp` and `coretemp` drivers:
Zen (family 17h to 1Ah) with per-CCD temperatures on Zen 2 through Zen 5
desktop and server parts, and Intel Core (family 6) package temperature
and power. A CPU outside that is left unknown rather than guessed at; a
confident wrong temperature is worse than none. On the Ryzen 9 9900X it
was written against, its readings were taken beside LibreHardwareMonitor's
at the same moments: Tctl agreed to a tenth of a degree, CCD1 within one
step, and CCD2 and package power within the second between the two
samples. On any other CPU that comparison is worth making once, and a
disagreement is a bug report.

`BUSY` needs no driver and is the one that answers the question the rest
cannot: whether a slow thread is hashing slowly or not being run. It is the
thread's own CPU time over wall time between two API calls, by
`GetThreadTimes` or `pthread_getcpuclockid`. A hasher with its logical CPU
to itself reads 100 whatever its rate; one at a third of its neighbours'
rate, at the same clock, with `BUSY` at 35, is being pre-empted — by
something at a higher priority on the same logical CPU, which on a desktop
is usually whatever else the machine is doing — and no amount of tuning the
hash loop will change that. The page flags a CPU thread under 90.

The miner says once in its log where each reading came from, or what it
looked for and did not find, so a rig with no temperatures can read why.

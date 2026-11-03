# avian-miner on HiveOS

HiveOS has no built-in entry for this miner, so it installs as a **custom
miner**: one tarball that HiveOS downloads, unpacks into
`/hive/miners/custom/avian-miner/` and drives through four scripts in it.

The release publishes that tarball beside the ordinary Linux one:

| asset | for |
| --- | --- |
| `avian-miner-<version>-linux-x86_64.tar.gz` | any Linux machine, unpack and run |
| `avian-miner-<version>.tar.gz` | HiveOS, as a custom miner |

They hold the same binaries. The HiveOS one differs only in its name and
layout, and both of those matter.

**Do not rename that file.** HiveOS takes the miner's identity from the file
name rather than from the manifest inside: it splits the name at the last
hyphen, treats what comes before as the miner and what comes after as the
version, and then insists the archive hold one top-level directory called
exactly that miner name. `avian-miner-1.1.0.tar.gz` is read as `avian-miner`
at version `1.1.0`, and the directory inside is `avian-miner`, which is why
it works. Adding anything after the version, such as `-hiveos`, makes HiveOS
look for a directory named `avian-miner-1.1.0` and the install fails with
`No avian-miner-1.1.0/h-manifest.conf`.

## Setting up a flight sheet

Create a flight sheet with **Miner: Custom**, then:

| field | value |
| --- | --- |
| Miner name | `avian-miner`, which HiveOS fills in from the URL |
| Installation URL | the `avian-miner-<version>.tar.gz` URL from the release page |
| Hash algorithm | `x16rt` for GPUs, `minotaurx` for CPUs |
| Wallet and worker template | `%WAL%.%WORKER_NAME%`, or your address and rig name |
| Pool URL | `stratum+tcp://host:port` |
| Pass | usually `x` |

Nothing else is required. The miner name must be exactly `avian-miner`,
because HiveOS derives the install directory from it and the scripts inside
the package expect to be there.

**`x16rt` turns the GPU on for you.** It has a CPU implementation too, and
would otherwise quietly use it at a thousandth of the rate, which on a rig
looks like the cards have failed rather than like a missing flag. The
generated config therefore carries `"gpu": true` whenever the algorithm is
`x16rt`, and says so in the setup output.

## Extra config arguments

The box takes **one JSON fragment a line**, merged into the generated config
in order, so a later line wins and anything here overrides the defaults. The
keys are the miner's long option names without the dashes:

```
"devices": "1"
"gpu-backend": "cuda,amd"
"quiet": true
```

That is the same shape HiveOS's own cpuminer-opt integration uses, because
this miner takes the same JSON config.

`"devices"` is worth knowing about on a mixed rig: it picks which cards to
use, numbered as the miner numbers them, which is **not** the order HiveOS
shows. Run `./avian-miner --list-devices` over SSH in the install directory
to see the numbering before you set it. It prints the PCIe bus beside each
card, in hex, so the list lines up with `amd-info`, `nvidia-info` and
`lspci`:

```
GPU #0: NVIDIA GeForce RTX 3070 (Nvidia) on PCI bus 05
GPU #1: AMD Radeon RX 6600 [gfx1032] (OpenCL) on PCI bus 08
```

## What the stats show

`h-stats.sh` reads the miner's own API over loopback once a cycle and reports
the total hash rate, a rate per mining thread, accepted and rejected shares,
uptime, the algorithm and the version.

**Per-card rates are mapped by PCIe bus**, not by position in a list. The
miner runs one thread per GPU and reports the bus each one is on, which
HiveOS pairs with its own GPU table. This matters on a mixed rig: the miner
numbers CUDA cards before OpenCL ones, HiveOS follows the bus, and the two
coincide only by luck. If a backend cannot report a bus, the mapping is
omitted rather than guessed, and the rig page shows the total alone.

Temperatures and fans for a GPU run are not reported here. They belong to the
cards, and the HiveOS agent reads them from the driver, which is a better
source than a miner. A CPU run does report its one temperature.

The API is bound to `127.0.0.1` and `--api-remote` is never passed. It is an
unauthenticated control socket that can change the pool and stop the miner,
so it should not be reachable from the network.

## Checking a card before trusting it with shares

The package carries `avian-ocl-selftest` and `avian-ocl-batchtest`. On a card
that has never run these kernels, or after changing an overclock, SSH in and:

```sh
cd /hive/miners/custom/avian-miner
./avian-ocl-batchtest --list
./avian-ocl-batchtest --device N
```

19 chain orders, every candidate re-hashed on the CPU, and `0 with a problem`
is the answer you want. This is the check that catches an unstable overclock,
which does not crash: it computes wrong hashes, and on a pool that looks like
bad luck for hours. Note that these tools number devices the way the OpenCL
loader does, which is a third numbering again, so take `N` from their own
`--list` and not from the miner's.

A batchtest run is **not** a load measurement. It reports around a third of
GPU load where the miner on the same card reports 99%, because it is a single
batch inside a one-second process.

## When it will not start

HiveOS keeps the log at `/var/log/miner/avian-miner/avian-miner.log`.

- **"no config"**: `h-config.sh` did not run. Apply the flight sheet again.
- **"a GPU backend loaded and found no card"**: the runtime is there and
  enumerated nothing. Check the log just above for a line about
  `CUDA_VISIBLE_DEVICES` or similar, which HiveOS sets for some miners and
  which hides cards from this one too.
- **No AMD card listed**: HiveOS ships ROCm's OpenCL userspace and no HIP, so
  AMD cards are driven through OpenCL. That is the default. If you have
  forced `--gpu-backend=cuda,amd`, HIP will not load here.
- **A card appears as `gfx1032` rather than by name**: older builds printed
  what ROCm calls the device, which is the architecture. Current builds ask
  for the board name as well and print `AMD Radeon RX 6600 [gfx1032]`.

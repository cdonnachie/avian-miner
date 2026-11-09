# avian-miner

A miner for [Avian](https://avn.network), covering both of its proof of work
algorithms on CPU and GPU, over Stratum v1 and v2.

Forked from [cpuminer-opt](https://github.com/JayDDee/cpuminer-opt) by
Jay D Dee, which is where most of this miner comes from. What Avian adds is a
GPU backend for X16RT and a Stratum v2 client.

## What runs where

| Algorithm | CPU | NVIDIA GPU | AMD GPU |
| --- | --- | --- | --- |
| X16RT | yes | yes, CUDA | yes, HIP, tested on one card so far |
| MinotaurX | yes | no | no |

MinotaurX is memory hard and has no GPU backend, and `--gpu` is refused
rather than quietly mining on the CPU.

`-a` takes four names and nothing else, where upstream offers ninety:

| `-a` | what it is |
| --- | --- |
| `minotaurx` | Avian, on the CPU |
| `x16rt` | Avian, on the GPU |
| `minotaur` | MinotaurX without its yespower node, for measurement |
| `yespower` | that node alone, with `-N 2048 -R 8 -K 'et in arcadia ego'` |

The last two mine nothing. They are kept because the pair is what measures
how much of a MinotaurX nonce is memory hard, which turns out to be 98.4%.

**On AMD, what has actually been run.** Three cards. An RX 6900 XT
(`gfx1030`, RDNA2) hashes x16rt at 27.3 MH/s through OpenCL against 25.3
through HIP, an Instinct MI325X (`gfx942`, CDNA3) at 107 against 83.5, and an
RX 6600 (`gfx1032`, RDNA2) at 8.9 on a HiveOS rig, which ships no HIP to
compare against and is the case this backend was written for. Nothing any of
them reported failed to re-hash on the CPU, in any of the five runs. OpenCL
being the faster wherever both ran is why `--gpu` sends an AMD card there and
leaves HIP to the machines that have no OpenCL runtime. That is still two of
the sixteen targets the libraries are built for, both RDNA2: RDNA3, RDNA4 and
the APUs compile and load but have not hashed on real hardware. Note also that `gfx942` is not in
`cuda/hip/gfx-targets`, so an Instinct needs `--with-hip-arch=gfx942` and a
build from source. Check your shares are being accepted before leaving an
untried card running.

**On HiveOS**, install the `avian-miner-<version>.tar.gz` asset as a custom
miner rather than the `-linux-x86_64` one: same binaries, but the name and
layout HiveOS parses and the four `h-` scripts it drives. AMD cards there go through OpenCL, which is what the
image ships. See [doc/hiveos.md](doc/hiveos.md).

**Intel GPUs are plausible and untested.** The OpenCL backend recognises
`intel` as a vendor and nothing in it is AMD- or Nvidia-specific, so an Arc
card or an integrated GPU should enumerate and run; neither CUDA nor HIP will
claim it, so OpenCL gets it by default. But no Intel device has ever run these
kernels, and "should" is not a measurement. If you have one, `avian-ocl-selftest`
and `avian-ocl-batchtest` answer the question in about a minute, and the
result is worth sending in either way.

**Only the backend knows about your card.** The miner loads `avian_cuda.dll`
and `avian_amd.dll`, or `libavian_cuda.so` and `libavian_amd.so`, at run time
rather than linking them, across an ABI of plain data. So building one for your
own hardware and dropping it in beside the binary is the whole job, with
nothing else to rebuild:

```powershell
scripts\windows\build-cuda-dll.ps1 -Arch 86          # one Nvidia target
scripts\windows\build-hip-dll.ps1  -Arch gfx1100     # one AMD target
```

A few megabytes and a couple of minutes, against forty megabytes and the
better part of an hour for every architecture the release carries. The loader
checks each library's ABI version, so one built against a different version of
the miner is skipped with a line saying so rather than doing anything worse.
On Linux the equivalent is `--with-cuda-arch=86` or `--with-hip-arch=gfx1100`
at configure time; see [doc/building.md](doc/building.md).

The two platforms need different things installed. On Windows the Adrenalin
driver's own `amdhip64_6.dll` is enough. On Linux `libavian_amd.so` needs
ROCm's HIP runtime, `libamdhip64.so.6` and `libhsa-runtime64.so.1` among
others, which the display driver alone does not provide. Without them the
backend is skipped with a line saying so, and the miner runs on whatever else
it found.

**The Linux release carries no AMD backend.** It is built in a container with
no ROCm in it, so that the binary runs on a distribution older than the build
host rather than only on it. An AMD card on Linux therefore means a build from
source with `--with-hip`, and the backend's `RUNPATH` starts with `$ORIGIN` so
the HIP runtime can sit beside it on a machine where ROCm cannot be installed.
[doc/building.md](doc/building.md) covers both routes. Windows is unaffected
and ships `avian_amd.dll` as before.

## Running it

One binary serves either vendor, both, or neither. It loads `avian_cuda.dll`
and `avian_amd.dll` (`libavian_cuda.so` and `libavian_amd.so` on Linux) from
its own directory at startup and uses whichever are there and will load, so
a machine missing one vendor's driver simply has fewer devices rather than a
miner that will not start.

```sh
# CPU, MinotaurX
avian-miner -a minotaurx -o stratum+tcp://POOL:PORT -u ADDRESS.worker

# GPU, X16RT
avian-miner -a x16rt --gpu -o stratum+tcp://POOL:PORT -u ADDRESS.worker

# Stratum v2, against a pool that speaks it
avian-miner -a x16rt --gpu --sv2=POOL:PORT --sv2-user=ADDRESS.worker \
            --sv2-authority=POOL_AUTHORITY_KEY
```

Every GPU device is numbered in one list, Nvidia first, so `GPU #2` means the
same thing whether this machine has Nvidia cards, AMD cards or both.
`--devices 0,2` or `--devices 0-2` mines on a subset of them, numbered by that
same list, and `--list-devices` prints the numbering and exits, so finding out
which card is which costs no mining run.

Stratum v2 is built on [avian-sv2](https://github.com/cdonnachie/avian-sv2),
a separate repository. A miner built without it still mines over Stratum v1;
see [doc/building.md](doc/building.md).

Without `--sv2-authority` the Stratum v2 connection is encrypted but
unauthenticated: the handshake proves nothing about who is on the other end,
so anything able to reroute your traffic can take the hashrate. Pools that
speak v2 publish the key.

A pool running [avian-sv2-solo](https://github.com/cdonnachie/avian-sv2)
takes a donation as part of the identity rather than as an option of its own,
by prefixing it with `donate/<percent>/`:

```sh
avian-miner -a x16rt --gpu --sv2=POOL:PORT \
            --sv2-user=donate/2/ADDRESS.worker \
            --sv2-authority=POOL_AUTHORITY_KEY
```

The percentage takes up to two decimal places, so `donate/2/`, `donate/0.5/`
and `donate/12.34/` all work, and it adds to whatever fee the operator
charges. One that does not parse refuses the channel rather than being read
as zero, so a typo stops the miner instead of quietly changing what you are
paid. If the operator has set no address to pay donations to, the pool logs
that and you keep everything; the miner cannot see that happen, so an
accepted prefix and an ignored one look the same from here. Without the
prefix nothing is taken: avian-miner has no fee of its own, and no default.

## Watching it

`--api-bind 127.0.0.1:4048` opens the miner's own API, a plain text protocol
answering `summary`, `threads` and `blocks` one command per connection. It is
off unless asked for, which on HiveOS `h-config.sh` does for you and nowhere
else does.

`avian-webui`, which ships beside the miner, is a read-only web dashboard
that reads it and serves a page the rest of the LAN can open: hash rate and
its trend, a row per card, and the recent blocks with the card that found
each one. It carries its page inside itself, so it is one file with nothing
to install, and it is a separate process by design — a miner should not also
be an HTTP server. See [doc/webui.md](doc/webui.md).

## Verifying a release

Windows binaries are Authenticode signed and timestamped. Check before
running something that will use your hardware for hours:

```powershell
Get-AuthenticodeSignature .\avian-miner.exe | Format-List Status, SignerCertificate
```

`Status` should be `Valid`. The backend DLLs are signed too, which matters
here more than usual: the kernels are loaded at run time rather than linked,
so a signature on the executable alone says nothing about the code that
computes every hash. The bundled MSYS2 runtime DLLs are unsigned, because
they are other people's work and signing them would assert something about
OpenSSL and libcurl that is not meant.

## Building

See **[doc/building.md](doc/building.md)**. A CPU-only build is upstream's
`./build.sh`. The GPU build is not, and on Windows it is split across two
toolchains for reasons that document explains.

Briefly: nvcc will only use MSVC as its host compiler, while this miner and
[avian-sv2](https://github.com/cdonnachie/avian-sv2), its Rust Stratum v2
library, are MinGW, so the kernels live in a separate library that the miner
loads rather than links.

## Antivirus

Miners are often flagged by antivirus software. This is a false positive, and
it happens because the program is a miner, not because of anything it does.
The source is here to inspect. If you do not trust it, build it yourself or
do not run it.

## Credit and bug reports

Most of this code is Jay D Dee's cpuminer-opt, which is itself a fork of
cpuminer-multi by TPruvot, with work from Lucas Jones, djm34, Wolf0, pooler,
Jeff Garzik, ig0tik3d, elmad, palmd and Optiminer. The CUDA kernels derive
from suprminer. Upstream's README is kept at
[README-cpuminer-opt.md](README-cpuminer-opt.md) and lists the ninety or so
algorithms it supports. This fork offers four of them, so treat that document
as history rather than as this miner's manual.

**Report bugs about this fork here**, not to upstream. A problem report needs
the command line, the miner's startup output including the banner and device
lines, and whether it worked before.

## License

GPL version 3. The vendored CUDA kernels are GPL-3, and cpuminer-opt is GPL-2
or later, so the combined work is distributed under GPL-3. The full text is in
[COPYING](COPYING); [doc/licensing.md](doc/licensing.md) says why the version
differs from upstream's.

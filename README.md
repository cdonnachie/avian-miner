# avian-miner

A miner for [Avian](https://avn.network), covering both of its proof of work
algorithms on CPU and GPU, over Stratum v1 and v2.

Forked from [cpuminer-opt](https://github.com/JayDDee/cpuminer-opt) by
Jay D Dee, which is where most of this miner comes from. What Avian adds is a
GPU backend for X16RT and a Stratum v2 client.

## What runs where

| Algorithm | CPU | NVIDIA GPU | AMD GPU |
| --- | --- | --- | --- |
| X16RT | yes | yes, CUDA | builds and loads, **never tested on a real card** |
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

**On AMD:** the kernels compile for RDNA2, RDNA3 and RDNA4, the library loads
and reports the right ABI, and the dispatch path is covered by a test. No AMD
card has ever hashed with it. Treat it as untested until someone reports
otherwise, and check your shares are accepted before leaving it running.

The two platforms need different things installed. On Windows the Adrenalin
driver's own `amdhip64_6.dll` is enough. On Linux `libavian_amd.so` needs
ROCm's HIP runtime, `libamdhip64.so.6` and `libhsa-runtime64.so.1` among
others, which the display driver alone does not provide. Without them the
backend is skipped with a line saying so, and the miner runs on whatever else
it found.

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
same thing whether this machine has Nvidia cards, AMD cards or both. There is
no way to mine on a subset of devices yet.

Without `--sv2-authority` the Stratum v2 connection is encrypted but
unauthenticated: the handshake proves nothing about who is on the other end,
so anything able to reroute your traffic can take the hashrate. Pools that
speak v2 publish the key.

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
its Rust Stratum v2 library are MinGW, so the kernels live in a separate
library that the miner loads rather than links.

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
or later, so the combined work is distributed under GPL-3. See
[COPYING](COPYING).

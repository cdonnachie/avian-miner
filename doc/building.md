# Building avian-miner

Two platforms, two quite different procedures. Linux builds the CUDA kernels
straight into the binary; Windows cannot, and the reason shapes everything
else on that side.

- [What you need](#what-you-need)
- [Linux](#linux)
- [Windows](#windows)
- [Why Windows is split in two](#why-windows-is-split-in-two)
- [Regenerating the driver](#regenerating-the-driver)
- [Known rough edges](#known-rough-edges)

A CPU-only build needs none of the CUDA apparatus below:

```sh
./build.sh        # autogen, configure --with-curl, make
```

That is upstream's script and it still works. Everything else here is about
the GPU backend.

## What you need

| | Linux | Windows |
| --- | --- | --- |
| Compiler | gcc | MSYS2 MINGW64 gcc, **and** MSVC 2022 |
| CUDA | toolkit under `/usr/local/cuda-12.9` (12.8 or 12.6 also accepted) | toolkit at `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9` |
| Libraries | libcurl, jansson, gmp | `mingw-w64-x86_64-jansson` (the script installs it) |
| Optional | Rust, for the Stratum v2 backend | `mingw-w64-x86_64-rust` (the script installs it) |

CUDA 12.8 is the first release with Blackwell support, which is why
`configure-build.sh` prefers the newest 12.x it finds rather than the first.

The Stratum v2 library is a separate repository, expected at
`/c/development/avian-sv2` on Windows or `$HOME/avian-gpu/avian-sv2` on
Linux. Without it the build still succeeds, but `--sv2` and
`--avian-selftest` are compiled out, and since the pool speaks only Stratum
v2 a miner without it cannot reach the pool.

## Linux

A plain build works, because `--with-cuda` is part of the committed
`configure.ac`:

```sh
./autogen.sh
./configure --with-curl --with-cuda=/usr/local/cuda-12.9 --with-cuda-arch=86
make -j"$(nproc)"
```

`--with-cuda-arch` is optional and takes a space-separated list. Left out,
configure asks the toolkit what it can emit with `nvcc --list-gpu-arch` and
builds every architecture from Maxwell to Blackwell that it offers, which is
the right thing for a release and about an hour you do not want while
iterating.

For development, one script does the whole thing including the driver
regeneration and the selftests:

```sh
scripts/configure-build.sh
```

It is not a thin wrapper around configure. In order, it:

1. picks the CUDA toolkit, newest 12.x first;
2. applies the device-tuning patches to `cuda/device.cu`;
3. **regenerates** `algo/x16/x16rt-cuda.cu` from `vendor/suprminer-jdd` and
   splits it into `algo/x16/x16rt-cuda-batch.cu`, then asserts the fixes are
   present in the output, because twice a build has quietly used the previous
   driver and a silently stale driver is worse than a failed one;
4. checks that `Makefile.am`'s `CUDA_SOURCES` still matches the kernels on
   disk, and stops if it does not;
5. builds `libavian_sv2.a` if the workspace is present and it has not been
   built;
6. `make distclean`, `autogen.sh`, `configure`, `make -j`;
7. asserts the GPU symbols are in the binary, then runs `--avian-selftest`
   on CPU and GPU.

Useful variables:

```sh
CUDA_ARCH=all scripts/configure-build.sh   # release arch list, not just sm_86
CLEAN=0       scripts/configure-build.sh   # skip distclean while iterating
MINER=/path/to/clone scripts/configure-build.sh
```

`CUDA_ARCH` defaults to `86`, one architecture, because compiling the full
list for every iteration is slow. Use `all` for anything you intend to ship.

Note step 6 does a `distclean` by default, and that is deliberate.
Reconfiguring changes per-file compiler flags while make only rebuilds what
it thinks is stale, so a half-rebuilt tree ends up holding objects from two
configurations. It surfaces as undefined references to things like
`V512_BSWAP32`, which look like missing code and are not.

## Windows

Six stages: two in MSYS2's MINGW64 shell, two from PowerShell, then two more
in MINGW64. They must run in this order, each depending on the last.

```sh
# MSYS2 MINGW64
scripts/windows/win-build.sh     # 1. fresh clone, CPU miner
scripts/windows/win-sv2.sh       # 2. add the Stratum v2 backend
```

```powershell
# PowerShell, because nvcc needs MSVC
wsl -- bash /mnt/c/development/cpuminer-avian/scripts/windows/prep-windows-cuda.sh
scripts\windows\build-cuda-dll.ps1
```

```sh
# MSYS2 MINGW64
scripts/windows/win-gpu.sh       # 5. link the DLL into the miner
scripts/windows/win-package.sh   # 6. collect the DLLs for distribution
```

What each one is for:

**`win-build.sh`** clones the repository into a scratch directory and builds
there rather than in your working tree, because that tree holds object files
from Linux builds and make would mix them.

**`win-sv2.sh`** builds `libavian_sv2.a` with MinGW Rust and reconfigures.
`CARGO_TARGET_DIR` is kept outside the checkout: a cargo `target/` there runs
to gigabytes and the Docker build contexts walk it.

**`prep-windows-cuda.sh`** re-applies six patch steps to `cuda/**`: POSIX
compat, the DLL shim, the AES barriers, the skein abort, and the portable
`lop3` forms. Each is idempotent. Skipping the whole stage has a specific
failure signature: the ECHO and SHAVITE chains disagree with Linux at 12 to
26 percent while Linux stays clean.

Its first step copies `cuda/**` out of a Linux clone, and that step is
optional. It is worth having when you are changing kernels in the Linux tree
and want them on Windows before committing. With no Linux tree present it
reports the tracked files instead and carries on, so a clean Windows
checkout builds the DLL on its own. `L` and `W` override the two paths.

**`build-cuda-dll.ps1`** compiles every `.cu` in one nvcc invocation, so
device linking is implicit and there is no separate `-dlink` step to get
wrong. It takes one switch:

```powershell
scripts\windows\build-cuda-dll.ps1 -NoPtx
```

`-NoPtx` builds the plain-C form of all 60 `lop3.b32` sites instead of the
inline PTX, which is what hipcc would select on its own. It exists so the
portable path can be verified on Nvidia hardware before anyone owns an AMD
card. The resulting DLL is larger, around 12MB against 9.4MB, which is one
instruction becoming several and is why the asm stays the default.

**`win-gpu.sh`** appends the gate and host half to `Makefile.am`, reruns
automake, reconfigures with `-DHAVE_CUDA`, and `make clean`s before
rebuilding. Both of those last two matter. `Makefile.am` is input to
automake, not make, so without the rerun the edit never reaches `Makefile.in`
and the build reports OK while producing a byte-identical binary with no GPU
backend. And automake does not make objects depend on `CPPFLAGS`, so without
the clean the miner's own code stays compiled without `HAVE_CUDA`: the binary
then links the DLL and still refuses `--gpu` with "this build has no GPU
backend". The script asserts against exactly that, by looking for the string
in the artifact.

**`win-package.sh`** walks the import tables breadth-first. The first level is
short, but MSYS2's libcurl pulls in OpenSSL, zstd, brotli, nghttp2 and idn2.
Walking only direct imports produces a zip that works on the build machine
and nowhere else.

Overridable paths, all defaulting into a scratch directory:

```sh
WORK=/c/builds/avian scripts/windows/win-build.sh
SRC=/path/to/clone   scripts/windows/win-build.sh
```

## Why Windows is split in two

nvcc will only use MSVC as its host compiler. The miner and its Rust Stratum
v2 library are MinGW. There is no single toolchain that builds both, so the
kernels live in `avian_cuda.dll`, built by nvcc with MSVC, and the miner
links its import library.

That boundary is only survivable because it carries plain data and nothing
else: see `cuda/avian-cuda-api.h`. The miner needs no CUDA headers, no nvcc,
and no knowledge of the kernels. It is also why `win-gpu.sh` adds just two
source files and a `.lib`.

## Regenerating the driver

Only needed when changing the generator or moving to a new upstream. The
generated `algo/x16/x16rt-cuda-batch.cu` is tracked, so an ordinary build
never does this.

```sh
python3 scripts/gen-driver.py vendor/suprminer-jdd/x16r/x16r.cu \
                              algo/x16/x16rt-cuda.cu
python3 scripts/make-dll-driver.py algo/x16/x16rt-cuda.cu \
                                   algo/x16/x16rt-cuda-batch.cu
```

`algo/x16/x16rt-cuda.cu` is an untracked intermediate. The upstream input is
vendored in tree; `vendor/suprminer-jdd/README.avian.md` records the
revision and why it is a copy rather than a submodule.

**Never hand-edit `algo/x16/x16rt-cuda-batch.cu`.** It is generated, and an
edit there evaporates at the next `configure-build.sh` run with nothing to
say it has gone. Changes belong in `scripts/gen-driver.py`,
`scripts/add-tracer.py` or `scripts/make-dll-driver.py`. The same rule covers
`cuda/upstream/**`, where changes are expressed as the idempotent patch
scripts in `scripts/` and re-applied at staging time.

## Known rough edges

Recorded because each one has already cost a debugging session, or would.

**`Makefile.am`'s kernel list is written down, not globbed.** That is
deliberate, so what gets built is what is in the tree rather than whatever
happens to be on disk. The cost is that adding a `.cu` under `cuda/upstream`
needs the list updating too. `scripts/patch-build.py` compares the two every
run and fails loudly on a mismatch, naming the file, because a kernel that is
not compiled fails at link if you are lucky and at runtime if you are not.

**Two scratch paths are still hard-coded.** `$OUT` and `$S` in
`build-cuda-dll.ps1` point at a session temp directory with no environment
override. The four `win-*.sh` scripts have the same defaults but do accept
`WORK`, `B` and `REL`.

**`scripts/audit-aes.py` is not trustworthy.** It reports
`x16r/cuda_x16_echo512.cu:246` as needing a barrier when the file plainly has
one, which is why `prep-windows-cuda.sh` prints its output as informational
rather than failing the build on it. `fix-aes-barrier.py` places the barriers
deterministically and is the thing to rely on.

**`--avian-selftest` cannot judge a GPU.** It sets the target to all ones so
the first nonce wins, which suits a serial CPU loop. A GPU launches half a
million nonces at once and picks a winner with `atomicExch`, so the nonce it
reports is whichever thread wrote last. Any nonce satisfies an all-ones
target, so it reports an arbitrary one and disagrees with the node's hash for
the block's own nonce. What is worth reading from a GPU selftest run is only
whether it gets through all six blocks without stalling.

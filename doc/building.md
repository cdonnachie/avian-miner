# Building avian-miner

Two platforms, two quite different procedures. Linux builds the CUDA kernels
straight into the binary; Windows cannot, and the reason shapes everything
else on that side.

- [What you need](#what-you-need)
- [Linux](#linux)
- [Windows](#windows)
- [Why Windows is split in two](#why-windows-is-split-in-two)
- [Regenerating the driver](#regenerating-the-driver)
- [AMD, via HIP](#amd-via-hip)
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

## AMD, via HIP

It works, on both AMD architectures that matter. The miner builds, runs
and hashes correctly on an AMD Instinct MI325X, verified against the CPU
across the whole chain, and a Windows DLL built with
`scripts/windows/build-hip-dll.ps1` runs on consumer hardware: an RX 6900 XT
(`gfx1030`, RDNA2) hashes x16rt at 25.3 MH/s with no submission failing to
validate. Nothing in this section is needed for an Nvidia build.

Note which SDK, because it is not a detail: build consumer Windows kernels
against **6.x**, and ship no HIP runtime with them. The section below says
why, and getting it wrong fails silently on hardware that works.

```sh
./autogen.sh
./configure --with-curl --with-hip          # ROCm prefix defaults to /opt/rocm
make -j$(nproc)
```

`--with-hip` and `--with-cuda` build the same kernels for different hardware
and are mutually exclusive. The architecture list is asked of the machine
rather than defaulted, because there is no safe guess: a CUDA binary carries
PTX, so a card newer than the build still runs by driver JIT, whereas an AMD
binary built for the wrong `gfx` target simply fails to launch. Pass
`--with-hip-arch=gfx942` to build for a card that is not in the build
machine.

This is a smaller change to the build than it sounds, and the reason is the
Windows DLL split: the miner's own C never names a GPU runtime symbol, it
goes through the plain-data `avian_cuda_*` ABI in `cuda/avian-cuda-api.h`. So
the compiler and the runtime library are the whole of the difference, and
`HAVE_CUDA` keeps its name for a HIP build because it gates host code whose
every symbol is `avian_cuda_*` whichever card ends up running it.

### Which SDK version, and it is not the architecture list

Two separate things decide whether a Windows build runs on a given card. The
first is easy to mistake for the whole story, which is what happened here:
two packages were built and sent on that reasoning and both failed the same
way.

**The architecture list, which is the lesser one.** AMD's Windows support
matrix moves backwards as well as forwards. 7.2.0 marks `gfx1030`, the RX
6800, 6900 and 6950 XT and the PRO W6800, unsupported by both its runtime and
its SDK, where 7.1.1 and 6.2.4 both mark it supported by both. That is real,
but a card missing from the list fails to launch a kernel, which is not the
failure anyone was seeing.

**The runtime generation, which is the one that matters.** The kernel DLL
imports its HIP runtime by exact versioned filename, fixed at link time: a
7.x build imports `amdhip64_7.dll`, a 6.x build imports `amdhip64_6.dll`, and
only the matching file satisfies it. A consumer Windows machine does not have
the 7.x one. Measured on a tester's RX 6900 XT, driver dated 2026-10-15 and
thirteen days old at the time:

```
amdhip64.dll     present,  21,762,488 bytes
amdhip64_6.dll   present,  17,761,704 bytes
amdhip64_7.dll   absent
amd_comgr.dll    present, 105,432,984 bytes
```

So a current consumer Adrenalin driver supplies the 6.x HIP runtime and
nothing newer, and `gfx1030` being ticked or crossed on the 7.1.1 page was
never going to change that. The 6.4 SDK's own `amdhip64_6.dll` is 17,740,696
bytes, within 21 KB of the driver's, so the two are the same vintage.

**Build 6.x for consumer cards.** `build-hip-dll.ps1` takes `-Rocm` and `-Out`,
the latter so both generations can exist at once, since the import name makes
them different files rather than a runtime choice:

```powershell
.\build-hip-dll.ps1 -Rocm "C:\Program Files\AMD\ROCm\6.4" -Out ...\hip-6.4
```

6.4's device libraries cover every target the default list asks for:
`gfx1030`, `gfx1031`, `gfx1032` (RDNA2), `gfx1100`, `gfx1101`, `gfx1102`
(RDNA3) and `gfx1200`, `gfx1201` (RDNA4), as do 7.1.1's. Both also carry
`gfx1033` through `gfx1036` and `gfx1150` through `gfx1153`, the RDNA2 and
RDNA3.5 integrated parts, so adding those costs one compiler pass each and no
source changes. At roughly 1.9 minutes and 2.3 MB per architecture, the
default eight take about 16 minutes and produce a 21 MB DLL.

**Whether to ship the runtime inverts between the two**, and getting it the
wrong way round is silent both times.

| | 7.x | 6.x |
| --- | --- | --- |
| Driver supplies it | no | yes |
| Static imports | nothing AMD-specific | `amd_comgr_2.dll`, 121 MB |
| Ship it? | must | must not |

For 7.x the runtime has to travel with the kernels, because no consumer
driver has the file and without it the process dies at load with
`0xC0000135` having printed nothing. One 18 MB file is enough: its own
imports are SETUPAPI, KERNEL32 and friends, and it loads comgr lazily, only
to compile at run time, which this does not do.

For 6.x the runtime must be left out. 6.4's `amdhip64_6.dll` imports
`amd_comgr_2.dll` statically, so shipping it means shipping 121 MB beside
it or getting that same silent load failure, which is exactly what the first
6.4 package did. The driver's copy is already present, and is paired with the
driver's own comgr rather than one nobody tested against that kernel driver.
`package-amd-test.ps1 -NoRuntime` does this.

The filename is the trap in both directions. The runtime carries only its
**major** version, so 7.1.1 and 7.2.0 both produce `amdhip64_7.dll`:
interchangeable by name and not by content, with no link error and no
missing-DLL message when the wrong one is used. Both packaging scripts
therefore find the runtime by pattern inside the SDK prefix they were given
rather than naming the file.

### Diagnosing a card that is not found

A miner reporting no device gives one symptom for several unrelated causes,
and guessing between them cost two builds and two round trips with a tester.
`scripts/windows/package-amd-probe.ps1` builds a 6 MB zip that separates them,
shipping AMD's own two tools rather than ours, which is the point: if AMD's
probe cannot find the card either then nothing here is involved.

- `amdgpu-arch.exe` prints the `gfx` name. It loads `amdhip64.dll`, the
  **unversioned** name, so it uses the driver's own runtime and not anything
  shipped beside it. Note it prints `Failed to get device count` whether the
  runtime returned zero devices or could not be loaded at all, so a failure
  from it does not distinguish those two.
- `hipInfo.exe` asks the HIP runtime, needing only `amdhip64_7.dll`. It
  returns the **identical** `no ROCm-capable device is detected` string the
  miner prints, which is worth knowing: that message is AMD's, not ours.

It also records the driver version, the session type, which AMD runtime
components are installed and at what version, the same libraries in
DriverStore, the registered OpenCL vendors and the device's PnP problem code.
Two of those need explaining:

- **Remote sessions.** AMD's compute runtime cannot enumerate a GPU under
  Remote Desktop, because the session is handed a different display adapter.
  A healthy card reports no device. The probe checks `SESSIONNAME` first
  because it is the one answer that means nothing is wrong.
- **Absence from system32 is not absence.** Modern AMD drivers keep
  `amdocl64.dll` and `amdihk64.dll` in DriverStore and register them through
  the ICD registry, so finding them missing from system32 proves nothing. The
  probe looks in both before anyone concludes an install is broken.

One thing that does not work, in case it is reached for:
`HSA_OVERRIDE_GFX_VERSION` is read by ROCr on Linux and never by Windows HIP.
A retry built around it ran identically to the first attempt, and two
testers duly returned a `result.txt` whose halves were byte-identical.

### How it was taken this far without an AMD GPU

Two measurements, and the second only became possible once the first was
clean. Neither needs an AMD card, because a compiler does not need the
hardware it emits code for. On Windows, with only the HIP SDK installed:

```powershell
# 1. syntax, which is fast and finds the bulk of it
& "C:\Program Files\AMD\ROCm\7.1\7.1\bin\clang++.exe" `
    -x hip --offload-arch=gfx942 -fsyntax-only -std=c++14 -w `
    -Icuda\hip -I. -Icuda -Icuda\upstream `
    cuda\upstream\quark\cuda_bmw512.cu

# 2. object code, which is the one that counts
& "C:\Program Files\AMD\ROCm\7.1\7.1\bin\clang++.exe" `
    -x hip --offload-arch=gfx942 -c -O3 -std=c++14 -w -ferror-limit=1 `
    -Icuda\hip -I. -Icuda -Icuda\upstream `
    cuda\upstream\quark\cuda_bmw512.cu -o bmw512.o
```

Both are clean for every kernel, for `gfx942` (CDNA, the Instinct cards) and
`gfx1030` (RDNA) alike.

`cuda/hip/` is the shim: headers named `cuda_runtime.h`, `cuda.h` and so on
that include `hip/hip_runtime.h` and rename the CUDA runtime symbols the
kernels use. Everything in it is a rename. Where a construct needed the
kernels themselves to change, it lives in a patch script under `scripts/`
instead, and that split is what kept the port reviewable: the shim absorbs no
semantics. The `--with-hip` build puts this directory first on the include
path so a kernel's `#include <cuda_runtime.h>` finds it; an Nvidia build
never sees it.

### One fact explains most of it

**hipcc never defines `__CUDA_ARCH__`.** That single difference is both the
largest help and the cause of three separate problems.

It helps because the vendored headers already carry portable fallbacks for
their rotates and shifts behind `#ifdef __CUDA_ARCH__`, and HIP selects all
of them for free. Of 146 PTX mnemonic occurrences in the tree, 107 are
guarded that way and needed no work at all.

It hurts wherever upstream used the same macro to mean "host compiler":

- both helper headers declare `blockDim` and `threadIdx` for the host, which
  under HIP collide with its real builtins;
- `cuda_x13_hamsi512.cu` defines its 64x16 table twice, `__constant__` for
  the device and a plain host array otherwise, and HIP took the host one,
  giving "reference to `__host__` variable `d_T512` in `__global__` function".

`scripts/fix-hip-guards.py` widens those guards to say what they meant.
Nothing changes for CUDA, where `__HIPCC__` is undefined.

### What has been done

| Script | What it addresses |
| --- | --- |
| `make-ptx-portable.py` | all 60 `lop3.b32` sites, decoded from their own immediates |
| `fix-xor-asm.py` | 22 `xor.b64` statements in `xor1`/`3`/`5`/`8`/`9` |
| `fix-hip-guards.py` | the three `__CUDA_ARCH__`-as-host-compiler guards |
| `fix-launch-syntax.py` | 43 launches written `f << <grid, block >> > (args)` |
| `fix-remaining-ptx.py` | the last 12 inline PTX expressions |
| `fix-uint2-braces.py` | the two brace-elided `uint2` tables |

They are patch scripts rather than edits because `cuda/upstream` is vendored,
and `prep-windows-cuda.sh` re-applies all of them at staging time: a
re-vendor would otherwise drop the lot with no build failing to say so.

The launch one is worth a note: nvcc accepts the spaces and clang in HIP mode
does not, reporting `expected expression` about a line that is plainly a
launch. The normalised form is identical to nvcc, so it is applied
unconditionally rather than behind a flag. The `uint2` one likewise needs no
switch, since the braced form is what the rest of the tree already writes.

Everything else is selected by `AVIAN_NO_PTX`, which `cuda/avian-ptx-portable.h`
sets automatically under `__HIPCC__` and which can be forced on nvcc with
`-DAVIAN_NO_PTX=1`. That flag is the reason any of this is trustworthy, and
it is not a formality: see the verification section below.

Measured progress, as distinct error locations rather than error reports,
since one bad line in a shared header reports once per file that includes it:

```
47  starting point
27  after a shim for __builtin_align__, which alone accounted for 228 reports
22  after the launch syntax, the guards and the xor.b64 chains
16  after the hamsi table guard
 0  after the remaining PTX, the uint2 tables and the five runtime names
```

### Why a syntax check was not enough

Reaching zero above meant zero *syntax* errors. Compiling the same files to
object code then failed three of them, for two reasons that `-fsyntax-only`
is structurally unable to find, because it stops before instruction
selection.

HIP rejects the 64-bit `l` register constraint outright, which is why every
64-bit site announced itself early and why the list of remaining work looked
short. It **accepts `r`** and only fails later, in the backend. So:

- `cuda_jh512.cu` reported `illegal VGPR to SGPR copy`. Three bit
  permutations whose last instruction is `vshl.u32.u32.u32.clamp.add`, an
  Nvidia video instruction with no AMD counterpart at all.
- `cuda_helper_alexis.h` reported `invalid instruction` for `bfe.u32`, and
  `bfi.b32` directly below it was hidden behind `-ferror-limit=1`.

The lesson is worth keeping: for this codebase a HIP syntax check is a
progress indicator, not a gate. Only `-c` is a gate.

### Verification

The portable bodies are checked, not assumed. `-DAVIAN_NO_PTX=1` makes nvcc
compile the exact plain-C code an AMD card would run, so correctness is
testable on Nvidia hardware before any AMD card exists:

```
2 forced hash orders x 16 depths, GPU against CPU
  987648 comparisons, 0 differing
  894208 comparisons, 0 differing
```

That covers every stage of the X16RT chain. Separately, `avian_lop3` was
checked against an independent per-bit statement of the PTX rule for all 256
immediates over 307,200 combinations, 0 wrong.

The CUDA build is rebuilt and re-verified alongside, since these scripts edit
sources both backends share. The `-NoPtx` DLL is about 2.5 MB larger than the
default, which is one instruction becoming several and is why the asm stays
the default on Nvidia.

### Verified

On a gfx942 (MI325X, wave64, 304 CUs) the whole chain agrees with the CPU:

```
1F80675605446D40 : 6,557,184 comparisons, 0 differing
542CB1E146A7D915 : 5,474,048 comparisons, 0 differing
FE17C8172501B98C : 5,689,344 comparisons, 0 differing
```

seventeen and a half million comparisons over three orders and all sixteen
depths, with no launch failure, no memory fault and no setBlock error. Mining
against a fixed header for ninety seconds produced 83.5 MH/s and zero invalid
shares. Each algorithm's 80-byte kernel was checked on its own as well, by
forcing an order of one repeated nibble.

The defects that got it there, in the order they were found, because the
sequence is the useful part:

| what | how it showed |
| --- | --- |
| `jansson` missing from the `.cu` rule | build failure, masked on a host with jansson installed |
| HIP's `__device__ memcpy` shadowing the host one | build failure in five kernels |
| no `-rpath` on the HIP link | built, linked, died at startup |
| host and device disagreeing on block size | `unspecified launch failure` |
| `uint2 + <int literal>` broadcast element-wise | **ran, hashed wrong**, skein and bmw |
| host code reading a `__constant__` | **ran, hashed wrong**, hamsi and sha512 |
| a fence where a barrier was needed | **ran, hashed wrong, on Nvidia too** |

The last three are the ones worth remembering. They compiled, launched, and
produced plausible wrong answers, and no amount of compiling would have found
any of them.

Five of the seven trace back to one fact: HIP's `uint2` is a class template
where CUDA's is a plain struct, and `hipcc` never defines `__CUDA_ARCH__`.
Two were self-inflicted, by widening a guard with `__HIPCC__` where the host
pass needed excluding, and by converting every `__constant__` to a plain
`const`, which device code cannot address.

### Eliminated by measurement

None of this needs testing again. Each was measured on the card against a
reference written from the definition rather than copied from the headers:

- every uint2 helper in both header sets, `ROL2`, `ROR2`, `ROL8`, `ROR8`,
  `ROL24`, `SHL2`, `SHR2`, `xor3x`, `ROTL64` and `ROTR64` over both types,
  `shr_u64`, `shl_u64`, and the `vectorize` round trip. Exact.
- `uint2` `+`, `-`, `^`, `+=`, and `+` with a `uint32_t`. Exact. With an
  `int` literal it was not, which is the fix above.
- the packed `uint2x4` load and store: replacing both with element-wise
  access changed nothing.
- `__constant__` data four ways, including two translation units defining
  one name with different linkage and size. That matters because HIP links
  all device code into a single image where CUDA keeps a module per `.cu`,
  and this tree has ten definitions of `c_PaddedMessage80`.
- optimisation level. `-O1` and `-O3` behaved alike; `-O0` will not build,
  the scratch frame exceeding its limit.

### Still unmeasured

- **RDNA beyond one card and one chain order.** `gfx1030` has now run: an
  RX 6900 XT on a Ryzen 9 5950X did a minute of x16rt at 25.34 MH/s peak,
  intensity 19, with 176 submissions, none failing to validate and no
  faults. Every nonce the GPU found was re-hashed on the CPU before
  submission, so that is 176 independent agreements rather than merely an
  absence of crashes, and it closes the wavefront question for RDNA.

  What it does not cover: one chain order (`FE17C8172501B98C`) out of
  sixteen, and only the final hash rather than each of the sixteen depths.
  The CDNA verification did 17,720,576 comparisons across three orders and
  all sixteen depths; this is a working miner, not that. Running the harness
  on RDNA needs a card we control rather than a volunteer's, and
  `gfx1100` through `gfx1201` remain untouched by any hardware.
- **The `device_sm` block-size idiom**, meaningless off Nvidia and better
  deleted than translated.
- **Hashrate as a figure worth quoting.** 83.5 MH/s on CDNA and 25.3 on a
  6900 XT are what it does, not what it could do: the linker reports it
  cannot meet the `amdgpu-waves-per-eu` occupancy the launch bounds ask
  for, and the block sizes are still chosen from Nvidia SM versions. The
  consumer figure was also still climbing when the minute ran out, so even
  as a measurement it is a lower bound.
- **`ldg4`**, whose fallback copies one 32-byte structure where the PTX
  loads three. Nothing calls it.

### Both vendors at once, which is not built

Today a binary drives one vendor: the DLL beside it decides which. Running an
AMD and an Nvidia card in the same machine needs three things, none of them
large, and the ABI is already the right shape for all of them.

**Load the backends instead of importing them.** The miner links
`avian_cuda.lib`, so the loader binds `avian_cuda_scan_batch` to exactly
one DLL at startup, and two DLLs exporting that name cannot both satisfy a
static import. Loaded explicitly, the identical names stop being a collision
and become the point:

```c
HMODULE h = LoadLibrary("avian_amd.dll");
backend.scan_batch = GetProcAddress(h, "avian_cuda_scan_batch");
```

One ABI, two implementations, each behind its own table of five function
pointers. It also makes the miner degrade gracefully where a static import
cannot: a missing DLL, or a runtime whose vendor is not installed, becomes a
backend that is skipped rather than a process that will not start.

**Enumerate across both.** Concatenate the device lists, CUDA first, so
`--gpu` and the device numbering stay one namespace rather than two that
collide at zero.

**Route each thread.** Each GPU thread already owns one device index; it
needs to know which backend that index belongs to, which falls out of the
enumeration above.

Call it two to three hundred lines. The DLLs must then be separately named,
`avian_cuda.dll` and `avian_amd.dll`, since two files cannot share a name in
one directory; build-hip-dll.ps1 writes the former today because that is what
makes it a drop-in for testing, and changing it is one line.

Worth doing after a consumer AMD card has been shown to hash correctly, not
before. If RDNA turns out to miscompute, that wants isolating rather than
debugging through a new dispatch layer.

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

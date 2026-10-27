# Vendored CUDA kernels for X16RT

Upstream: <https://github.com/JayDDee/suprminer-jdd>, commit `e4efceafb2b92be83ae39be0b8ec910b20f26484`
(2022-02-02, "Merge x16rt.cu & x16r.cu into one source file"), licensed GPL-3.0.

Only the closure of what `scanhash_x16rt` reaches is here, under upstream's
own paths so a diff against upstream stays readable. Whole directories would
pull in algorithms this miner never runs, several of which use the texture
reference API that CUDA 12 removed.

suprminer is a fork of ccminer, which is a fork of cpuminer, which is what this
miner forks. That shared ancestry is why the kernels need so little from the
host: `struct work`, `applog`, `work_restart`, `fulltest` and `be32enc`
already mean the same thing on both sides.

## Licence

cpuminer-opt is GPLv2-or-later, so combining it with these GPLv3 kernels is
permitted and the resulting binary is GPLv3. That is a deliberate choice, not
an accident.

## Local modifications

Everything else is byte-for-byte upstream.

1. `#include "miner.h"` became `#include "avian-cuda-shim.h"` in 12 files.
   This miner has its own `miner.h`; the shim includes it and adds only the
   few names ccminer's header had that ours lacks.

2. `x16r/cuda_x16_fugue512.cu` no longer uses the texture reference API, which
   CUDA 12 removed. Its texture was a second copy of a table the same file
   already holds as `__device__ const dmixtab0[]` and reads with `__ldg`.
   The two tables were compared word by word and are identical, all 256, so
   redirecting the two reads is value-identical. The bind and free became
   no-ops.

## Deliberately not vendored

`x13/cuda_x13_fugue512.cu`. The x16rt path calls its `x13_fugue512_cpu_init`,
but that function only binds a texture, and the `_sp` variant the path
actually hashes with reads its own `static __device__ mixtab0[256]` through
`__ldg`. The init is vestigial, so the adapter drops the call rather than
carry a second file needing the texture change.

## Refreshing

```
scripts/vendor-cuda.sh
```

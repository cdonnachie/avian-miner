# suprminer-jdd, vendored

This is an unmodified copy of the upstream the Avian CUDA backend is derived
from. It is here as source, not as a submodule or a documented URL, because a
URL is only as durable as someone else's decision to keep a repository up.

| | |
| --- | --- |
| Upstream | <https://github.com/JayDDee/suprminer-jdd> |
| Revision | `e4efcea` "Merge x16rt.cu & x16r.cu into one source file" |
| License | GPL-3, see `LICENSE.txt` in this directory |
| Copied | 2026-10-27 |

Two things were left out of the copy: `.git`, since the revision is recorded
above, and the four prebuilt `*.lib` import libraries under `compat/`, which
this project never links and which have no business in a source tree.
Everything else is byte for byte what upstream published.

## What actually uses it

One file, `x16r/x16r.cu`. `scripts/configure-build.sh` hands it to
`scripts/gen-driver.py`, which produces `algo/x16/x16rt-cuda.cu`, and
`scripts/make-dll-driver.py` splits that into the tracked
`algo/x16/x16rt-cuda-batch.cu`.

Because that generated driver is tracked, **a plain build does not need this
directory at all**. It is needed only to regenerate, and keeping it means a
regeneration cannot silently draw from a different upstream than the
committed driver came from.

The kernels themselves came from the same upstream by a different route: they
were vendored into `cuda/upstream/` and are patched in place there by the
idempotent scripts in `scripts/`. Do not edit anything in this directory to
change kernel behaviour; nothing reads it except the generator.

## Licensing

Upstream is GPL-3. This project's own `COPYING` is GPL-2, but every source
file carries cpuminer-opt's "either version 2 of the License, or (at your
option) any later version", so the combination is distributable under GPL-3.
That is what the combined work is, and the copy here is part of the
corresponding source that obligation implies.

## Build systems in here are inert

This tree brings its own `configure.ac`, `Makefile.am` and `autogen.sh`. The
project's root `Makefile.am` sets `SUBDIRS = compat` and lists its sources
explicitly, so autotools never descends here and none of it is reachable from
a build. They are kept because the copy is verbatim.

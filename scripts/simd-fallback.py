#!/usr/bin/env python3
"""Hand SIMD-first epochs to the CPU instead of wasting the GPU on them.

Measured over several runs: chain orders beginning with 9 (SIMD) produce 0 good
candidates out of 397, while SIMD anywhere else in the chain is clean. So the
80-byte SIMD kernel is wrong and roughly one epoch in sixteen currently costs
the card 128 seconds of work that is entirely discarded.

Until that kernel is fixed, those epochs are worth more on the CPU than on the
GPU: a few MH/s of shares that count beats 40 MH/s of shares that do not.

Three edits:
  1. register_x16rt_algo picks the CPU scanhash first and only then hands the
     gate to the CUDA backend, so the backend can keep a pointer to it.
  2. register_x16rt_cuda_algo stores that pointer.
  3. the driver calls it when the chain starts with SIMD.
"""
import io
import os
import sys

root = sys.argv[1]


def edit(rel, pairs, skip_if):
    p = os.path.join(root, rel)
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    if skip_if in s:
        print("  %s already patched" % rel)
        return
    for old, new in pairs:
        if s.count(old) != 1:
            raise SystemExit("%s: expected 1 of %r, found %d" % (rel, old[:60], s.count(old)))
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("  patched", rel)


# ---- 1. move the CUDA hook to the end, so the CPU choice is made first ----
hook = """#if defined(HAVE_CUDA)
  /* --gpu replaces the CPU scan rather than adding to it. One algorithm runs
   * per process here, so a GPU run is wholly a GPU run, and the CPU is left
   * for the MinotaurX process alongside it. */
  if ( opt_gpu )
     return register_x16rt_cuda_algo( gate );
#endif
"""
new_hook = """#if defined(HAVE_CUDA)
  /* Last, not first: the gate now holds whichever CPU scanhash this build
   * selected, and the CUDA backend keeps a pointer to it so a chain the GPU
   * gets wrong can still be mined. */
  if ( opt_gpu )
     return register_x16rt_cuda_algo( gate );
#endif
  return true;
};
"""
edit("algo/x16/x16r-gate.c", [
    (hook, ""),
    ("""  opt_target_factor = 256.0;
  return true;
};

bool register_x16rt_veil_algo""", """  opt_target_factor = 256.0;
""" + new_hook + """
bool register_x16rt_veil_algo"""),
], skip_if="Last, not first")

# ---- 2. the backend keeps the CPU scanhash --------------------------------
edit("algo/x16/x16rt-cuda-gate.c", [
    ("   gate->scanhash = (void*)&scanhash_x16rt_cuda;",
     """   /* Kept before it is overwritten. The 80-byte SIMD kernel reports only
    * bad candidates, so a chain starting with SIMD is mined on the CPU
    * instead; see x16rt_cuda_set_cpu_fallback. */
   x16rt_cuda_set_cpu_fallback( (x16rt_scanhash_fn) gate->scanhash );
   gate->scanhash = (void*)&scanhash_x16rt_cuda;"""),
], skip_if="x16rt_cuda_set_cpu_fallback")

# ---- 3. declare the hand-off ---------------------------------------------
edit("algo/x16/x16rt-cuda.h", [
    ("""/* Same shape as the algo gate's scanhash, so it can be registered directly. */""",
     """/* The scanhash this build would have used without --gpu. */
typedef int (*x16rt_scanhash_fn)( struct work *, uint32_t, uint64_t *,
                                  struct thr_info * );

/* Where the GPU driver sends work it cannot do correctly. Measured: chains
 * beginning with SIMD yield 0 usable candidates out of 397, so they go here
 * until the 80-byte SIMD kernel is fixed. */
void x16rt_cuda_set_cpu_fallback( x16rt_scanhash_fn fn );

/* Same shape as the algo gate's scanhash, so it can be registered directly. */"""),
], skip_if="x16rt_scanhash_fn")

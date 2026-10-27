#!/usr/bin/env python3
"""Add the missing block barrier after the shared AES table is filled.

Run after re-vendoring the CUDA kernels. Upstream does not carry this fix.

The defect
----------
aes_gpu_init256_32 and its siblings fill an 8192-word shared table
cooperatively: each thread writes 32 words at threadIdx.x << 5, so exactly
256 threads cover it. The table is deliberately replicated 32 times so each
lane reads its own bank, which means a thread reads

    sharedMemory[(byte << 5) + lane]

written by thread `byte`, not by the thread doing the reading. Every read of
that table is therefore cross-thread, and the fill has to complete for the
whole block before any thread reads.

There is no __syncthreads() anywhere in cuda_x11_aes_sp.cuh, and of the call
sites only one has anything at all: cuda_x16_echo512.cu reaches for
__threadfence_block(), which is a memory fence and not a barrier. A fence
orders the calling thread's own accesses; it does nothing to wait for another
thread's writes. Somebody saw the hazard and picked the wrong primitive.

Pre-Volta this mostly survived, because warps in a block ran closely enough
in lockstep, which is why 2018 ccminer code ships with it. From Volta onward
independent thread scheduling lets warps progress freely, and on Ampere 12 to
26 percent of threads read an unwritten slice and compute a wrong hash. Any
chain containing ECHO or SHAVITE is affected, which is most chains.

Why the barrier goes at the call sites
--------------------------------------
Two sites guard the call with `if (threadIdx.x < 256)` because their blocks
are larger than 256 threads. A __syncthreads() inside a divergent branch is a
deadlock, so the barrier goes after the call, at block scope, where every
thread in the block reaches it.

cuda_x11_simd512.cu's guarded site is left alone: it already has a barrier
before SIMD_Compress, which is its AES consumer.

Anchored on the call text rather than line numbers, because line numbers
shift the moment anything above them changes, and a re-vendor is exactly when
that happens. Re-runnable: a site that already has a barrier is skipped.
"""
import io
import os
import re
import sys

BARRIER = (
    "\t/* The shared AES table is filled cooperatively and every read of it\n"
    "\t * is cross-thread, so the fill must complete for the whole block\n"
    "\t * before any thread reads. Without this, 12 to 26 percent of threads\n"
    "\t * on Volta and later read a slice nobody has written yet. */\n"
    "\t__syncthreads();\n")

FENCE_BARRIER = (
    "\t/* Was __threadfence_block(), which orders this thread's own accesses\n"
    "\t * and does not wait for any other thread's writes. Every read of the\n"
    "\t * shared AES table is cross-thread, so a fence was never the right\n"
    "\t * primitive here. */\n"
    "\t__syncthreads();\n")

# A call to one of the cooperative fills, with or without a threadIdx guard.
# The array name is captured because the table is not always read as
# name[...]: several kernels pass it to a device helper, so a read can look
# like aes_round_s(sharedMemory, ...) with no subscript in sight.
CALL = re.compile(
    r"^[ \t]*(?:if[ \t]*\([^)]*threadIdx\.x[^)]*\)[ \t]*)?"
    r"aes_gpu_init[A-Za-z0-9_]*\(([A-Za-z_][A-Za-z0-9_]*)\);[ \t]*\n",
    re.M)

FENCE = re.compile(r"^[ \t]*__threadfence_block\(\);[ \t]*\n", re.M)


def already_synchronised(s, after, name):
    """True if a barrier already stands between here and the first table use.

    Matching the array as a bare word matters: an earlier version looked for
    name[ and so saw no use at all in the kernels that hand the table to a
    helper, which made any unrelated __syncthreads() further down look like
    the barrier and skipped two real sites.
    """
    window = s[after:after + 2500]
    bar = window.find("__syncthreads()")
    if bar < 0:
        return False
    use = re.search(r"\b%s\b" % re.escape(name), window)
    return use is None or bar < use.start()


def patch(path):
    s = io.open(path, encoding="utf-8", errors="surrogateescape",
                newline="").read()
    out, last, added = [], 0, 0

    for m in CALL.finditer(s):
        out.append(s[last:m.end()])
        last = m.end()
        if already_synchronised(s, m.end(), m.group(1)):
            continue
        # A fence sitting where the barrier belongs is replaced, not kept.
        fm = FENCE.match(s, m.end())
        if fm:
            out.append(FENCE_BARRIER)
            last = fm.end()
        else:
            out.append(BARRIER)
        added += 1

    out.append(s[last:])
    if added:
        io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                newline="").write("".join(out))
    return added


def main():
    root = (sys.argv[1] if len(sys.argv) > 1
            else os.path.join(os.path.dirname(os.path.dirname(
                os.path.abspath(__file__))), "cuda", "upstream"))
    if not os.path.isdir(root):
        print("no such tree: %s" % root)
        return 1

    total, touched = 0, 0
    for dirpath, _dirs, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith((".cu", ".cuh")):
                continue
            p = os.path.join(dirpath, f)
            n = patch(p)
            if n:
                print("  %s: %d barrier(s)" % (os.path.relpath(p, root), n))
                total += n
                touched += 1
    if total:
        print("  %d barrier(s) across %d file(s)" % (total, touched))
    else:
        print("  every fill already has a barrier")
    return 0


if __name__ == "__main__":
    sys.exit(main())

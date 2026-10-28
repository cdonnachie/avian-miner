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
#
# shavite_gpu_init as well as aes_gpu_init, and its absence was not cosmetic.
# The three 80-byte shavite kernels fill their shared table through that name
# and follow it with __threadfence_block(), so they matched nothing here and
# never got a barrier. The result is a kernel that disagrees with the CPU for
# a few threads once in some thousands of batches, never reproducibly, on
# Nvidia as much as AMD: 6 failing batches in 1670 on a 3090 against about 1
# in 2000 on an MI325X. What identifies the device as the faulty side is that
# the CPU returns the same answer twice for a nonce the GPU answers
# differently on two runs.
CALL = re.compile(
    r"^[ \t]*(?:if[ \t]*\([^)]*threadIdx\.x[^)]*\)[ \t]*)?"
    r"(?:aes|shavite)_gpu_init[A-Za-z0-9_]*\(([A-Za-z_][A-Za-z0-9_]*)\);[ \t]*\n",
    re.M)

FENCE = re.compile(r"^[ \t]*__threadfence_block\(\);[ \t]*\n", re.M)

FUNC = re.compile(r"^[A-Za-z_][A-Za-z0-9_ *&:<>,]*\b(\w+)\s*\([^;{]*$", re.M)

# Kernels whose fill is already covered by a barrier the scope check cannot
# see. x16_simd512_gpu_compress_64_echo512 has a __syncthreads() at block
# scope before its AES consumer; adding a second one there is not merely
# redundant, it stopped the module loading and every later CUDA call failed
# with "invalid argument" in an unrelated kernel. Named rather than numbered
# so it survives a re-vendor.
SKIP = {"x16_simd512_gpu_compress_64_echo512"}


def enclosing_function(s, pos):
    """Name of the function the offset falls inside, best effort."""
    m = None
    for m2 in FUNC.finditer(s, 0, pos):
        m = m2
    return m.group(1) if m else ""


def already_synchronised(s, after, name):
    """True if a barrier at the fill's own scope stands before the first use.

    Two things this has to get right, both learned the hard way.

    Matching the array as a bare word: an earlier version looked for name[
    and so saw no use at all in the kernels that hand the table to a device
    helper, which let an unrelated barrier further down look like the right
    one.

    And scope. cuda_x11_shavite512_sp.cu has a __syncthreads() a few lines
    after the fill, but it is inside `if (thread < threads)`. Threads past
    the end of the batch never reach it, so it does not synchronise the
    block at all, and a barrier in divergent code is undefined behaviour
    besides. Counting braces and only accepting a barrier at depth zero is
    what tells the two apart; without it this reported three sites as
    already fixed and the disagreement rate went straight back to 53%.
    """
    # No window. A fixed lookahead can end before the first use of the
    # table, and then the search runs past the real boundary and finds a
    # barrier that actually belongs to later code. That is how three ECHO
    # sites were reported as already synchronised and shipped in a Windows
    # DLL without barriers, while audit-aes.py, which scans to the first use
    # with no limit, said FENCE ONLY for the same sites.
    window = s[after:]
    use = re.search(r"\b%s\b" % re.escape(name), window)
    limit = use.start() if use else len(window)

    depth = 0
    i = 0
    while i < limit:
        c = window[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == "_" and window.startswith("__syncthreads()", i):
            if depth <= 0:
                return True
            i += len("__syncthreads()")
            continue
        i += 1
    return False


def patch(path):
    s = io.open(path, encoding="utf-8", errors="surrogateescape",
                newline="").read()
    out, last, added = [], 0, 0

    for m in CALL.finditer(s):
        out.append(s[last:m.end()])
        last = m.end()
        if enclosing_function(s, m.start()) in SKIP:
            continue
        # Deterministic: a barrier goes after every fill except the one
        # documented exception, and the marker comment makes it idempotent.
        #
        # Deciding per site whether an existing barrier already covers the
        # fill needs to know both where the table is first used and at what
        # brace depth the barrier sits, and two attempts at that heuristic
        # disagreed with the audit and shipped a Windows DLL with three ECHO
        # sites unsynchronised. An extra barrier after a fill costs one
        # block-wide sync per kernel launch; getting it wrong costs 12 to 26
        # percent of the card on most chains. This is the cheap side of that
        # trade.
        following = s[m.end():m.end() + 500]
        if "shared AES table is filled" in following \
                or "Was __threadfence_block" in following:
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

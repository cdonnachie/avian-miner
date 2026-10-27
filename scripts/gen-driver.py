#!/usr/bin/env python3
"""Generate algo/x16/x16rt-cuda.cu from upstream's x16r.cu.

The 500 lines of kernel dispatch are copied verbatim, because a transcription
slip there would be a wrong hash rather than a build error. Only the boundary
is ours: the gate signature, submission, and verification against this miner's
own CPU X16RT instead of a second sphlib copy.
"""
import io
import os
import sys

SRC = sys.argv[1]
OUT = sys.argv[2]

src = io.open(SRC, encoding="utf-8", errors="surrogateescape", newline="").read()
lines = src.split("\n")


def rng(a, b):
    """Upstream lines a..b inclusive, 1-based."""
    return "\n".join(lines[a - 1:b])


def check(text, needle, n=1, what=""):
    found = text.count(needle)
    if found != n:
        raise SystemExit("expected %d of %r, found %d %s" % (n, needle[:60], found, what))


# Sanity: the ranges must still be what they were when they were mapped.
check(rng(37, 64), "static uint32_t *d_resNonce[MAX_GPUS];", 1, "(range A)")
check(rng(66, 105), "enum Algo {", 1, "(range B)")
check(rng(106, 108), "s_ntime", 1, "(range C)")
assert "static bool init[MAX_GPUS]" in lines[269], lines[269]
check(rng(1121, 1757), "extern \"C\" int scanhash_x16rt(", 1, "(range D)")

driver = rng(1121, 1757)

# --- 1. The gate signature. cpuminer-opt passes thr_info and a uint64_t
#        counter; ccminer passed thr_id and an unsigned long.
old_sig = ('extern "C" int scanhash_x16rt(int thr_id, struct work* work, '
           'uint32_t max_nonce, unsigned long *hashes_done)\n{\n')
new_sig = ('extern "C" int scanhash_x16rt_cuda( struct work *work, uint32_t max_nonce,\n'
           '                                    uint64_t *hashes_done,\n'
           '                                    struct thr_info *mythr )\n'
           '{\n'
           '        const int thr_id = mythr->id;\n')
check(driver, old_sig, 1, "(signature)")
driver = driver.replace(old_sig, new_sig)

# --- 2. The vestigial fugue init. It only bound a texture, and the _sp variant
#        the x16rt chain hashes with reads its own __device__ table.
old_fugue = "                x13_fugue512_cpu_init(thr_id, throughput);\n"
check(driver, old_fugue, 1, "(fugue init)")
driver = driver.replace(
    old_fugue,
    "                /* x13_fugue512_cpu_init is not called: it only bound a\n"
    "                 * texture, and x13_fugue512_cpu_hash_64_sp, which is what\n"
    "                 * this chain uses, reads its own __device__ table. */\n")

# --- 3. Hash order logging. This miner already prints the order with the
#        per-function colouring, so use that rather than a plain line.
#        Keyed on content: the statement is split over two lines and its
#        indentation is not worth guessing.
dl = driver.split("\n")
hits = [k for k, l in enumerate(dl) if 'applog' in l and 'hash order %s' in l]
if len(hits) != 1:
    raise SystemExit("expected 1 hash-order applog, found %d" % len(hits))
k = hits[0]
if "endiandata[17]" not in dl[k + 1]:
    raise SystemExit("hash-order applog continuation not where expected: %r" % dl[k + 1])
indent = dl[k][:len(dl[k]) - len(dl[k].lstrip())]
dl[k:k + 2] = [
    indent + "/* endiandata[17] is already the byte-swapped nTime that the",
    indent + " * CPU path passes here. */",
    indent + "x16rt_log_hash_order( hashOrder, endiandata[17] );",
]
driver = "\n".join(dl)

# --- 3b. A nonce range shorter than 256 masks down to zero, and then the scan
#         loop never advances. Reachable, not theoretical: --avian-selftest
#         asks for sixteen nonces, which hangs the loop launching empty
#         kernels at full CPU. 256 is the granularity the mask implies, so it
#         is the smallest honest floor.
old_mask = "        throughput &= 0xFFFFFF00; //multiples of 128 due to cubehash_shavite & simd_echo kernels\n"
check(driver, old_mask, 1, "(throughput mask)")
driver = driver.replace(
    old_mask,
    old_mask +
    "        /* The mask sends a range shorter than 256 nonces to zero, and\n"
    "         * then `pdata[19] += throughput` below cannot advance and the\n"
    "         * do-loop spins for ever on empty kernel launches. A caller\n"
    "         * asking for a handful of nonces, as --avian-selftest does, hits\n"
    "         * this. 256 is what the mask's own granularity implies. */\n"
    "        if (throughput < 0x100) throughput = 0x100;\n")

# --- 3c. The device-side result is cleared each pass, so its host mirror has
#         to be too. Upstream left work->nonces[0] holding the previous pass's
#         winner and relied on the caller replacing the whole work item; a
#         pass that finds nothing would otherwise resubmit the last one.
old_memset = "                CUDA_SAFE_CALL(cudaMemset(d_resNonce[thr_id], 0xFFFFFFFF, 2 * sizeof(uint32_t)));\n"
check(driver, old_memset, 1, "(resNonce memset)")
driver = driver.replace(
    old_memset,
    old_memset +
    "                /* Cleared with the device-side copy it mirrors. Left\n"
    "                 * alone, a pass that finds nothing still sees the\n"
    "                 * previous pass's nonce here and would offer it again. */\n"
    "                work->nonces[0] = UINT32_MAX;\n"
    "                work->nonces[1] = UINT32_MAX;\n")

# --- 3d. A chain beginning with SIMD goes to the CPU. The 80-byte SIMD
#         kernel reports only bad candidates: 0 usable out of 397 across three
#         SIMD-first epochs, while SIMD elsewhere in the chain is clean. One
#         epoch in sixteen starts this way and all of it is discarded.
old_sw = ("        char elem = hashOrder[0];" + chr(10) +
          "        const uint8_t algo80 = elem >= 'A' ? elem - 'A' + 10 : elem - '0';")
check(driver, old_sw, 1, "(algo80)")
driver = driver.replace(old_sw, old_sw + """

        /* Measured wrong, so not worth the card's time: a few MH/s of shares
         * that count beats forty of shares that do not. Remove this once the
         * 80-byte SIMD kernel is fixed. */
        if (0 && algo80 == SIMD && cpu_fallback)  /* off: measuring the swapped kernel */
        {
                static __thread uint32_t said = UINT32_MAX;
                if (said != s_ntime)
                {
                        said = s_ntime;
                        applog(LOG_INFO, "GPU #%d: this epoch's chain starts with SIMD, "
                                         "which this GPU path computes wrongly; mining it "
                                         "on the CPU instead", dev_id);
                }
                return cpu_fallback(work, max_nonce, hashes_done, mythr);
        }""", 1)

# --- 4. The found-nonce block. Everything above it, the kernel dispatch, stays
#        verbatim; this is the part whose conventions differ.
#        Both anchors carry their leading newline. Without it the end anchor
#        matches inside a more deeply indented copy of the same statement, in
#        the commented-out restart block, and the replacement stops early
#        leaving ccminer-only calls behind.
start = "\n                if (work->nonces[0] != UINT32_MAX)\n"
end = "\n                pdata[19] += throughput;\n"
if driver.count(start) != 1:
    raise SystemExit("expected 1 found-nonce block, got %d" % driver.count(start))
if driver.count(end) != 1:
    raise SystemExit("expected 1 advance statement, got %d" % driver.count(end))
i = driver.index(start) + 1
j = driver.index(end, i) + 1
# The span being discarded must be the one that was mapped, so a future
# upstream refresh that moves things fails here rather than silently.
span = driver[i:j]
for needle, n in (("work_set_target_ratio", 2), ("return res;", 1),
                  ("x16rt_hash(", 2), ("work->nonces[1]", 9)):
    if span.count(needle) != n:
        raise SystemExit("discarded span has %d of %r, expected %d"
                         % (span.count(needle), needle, n))
replacement = r'''                if (work->nonces[0] != UINT32_MAX)
                {
                        if (addstart) work->nonces[0] += start;

                        /* Check on the CPU before submitting, against this
                         * miner's own X16RT rather than a second
                         * implementation of it. That one is what
                         * --avian-selftest and avian/compare-with-avian-pow.sh
                         * already check against Avian Core, so a GPU result
                         * that disagrees with it is a kernel bug and must not
                         * reach a pool.
                         *
                         * endiandata holds the header as big-endian words, and
                         * be32enc of the GPU's nonce leaves word 19 in exactly
                         * the orientation x16r_hash expects: ccminer and this
                         * miner describe the same convention with the nonce
                         * integer byte-swapped between them, so the two
                         * cancel.
                         */
                        uint32_t _ALIGN(64) vhash[8];
                        uint32_t candidate[2];
                        int found = 0;

                        candidate[found++] = work->nonces[0];

                        /* A batch can hold a second solution. Upstream
                         * returned a count and left submission to the caller;
                         * here each candidate is checked and submitted on its
                         * own, so a mistake in picking the second one can only
                         * cost a share, never submit an invalid one. */
                        if (addstart)
                        {
                                uint32_t second = h_resNonce[thr_id][1];
                                if (second != UINT32_MAX)
                                        candidate[found++] = second + start;
                        }
                        else
                        {
                                uint32_t second = cuda_check_hash_suppl(thr_id, throughput,
                                                                       start, d_hash[thr_id], 1);
                                if (second != 0 && second != UINT32_MAX)
                                        candidate[found++] = second;
                        }

                        memcpy(x16r_hash_order, hashOrder, sizeof(x16r_hash_order));

                        for (int c = 0; c < found; c++)
                        {
                                if (c && candidate[c] == candidate[0]) continue;

                                be32enc(&endiandata[19], candidate[c]);

                                /* Prehashed per candidate rather than once per
                                 * batch. It covers the first 64 bytes, which
                                 * exclude the nonce, so once per batch ought to
                                 * be equivalent and costs less; this path runs
                                 * only when a candidate turns up, so the saving
                                 * is worth nothing and the certainty is worth
                                 * something. */
                                x16r_prehash(endiandata, pdata, x16r_hash_order);

                                /* x16r_hash_generic abandons the chain and
                                 * returns zero the moment work_restart is
                                 * set, leaving output untouched. That happens
                                 * whenever a new job lands while this batch
                                 * was on the GPU, and it is not a
                                 * disagreement: the candidate belongs to the
                                 * previous job and is worthless either way.
                                 * Treating it as one reported uninitialised
                                 * stack as though it were a hash, which is
                                 * how two different nonces came to print the
                                 * same value. */
                                if (!x16r_hash(vhash, endiandata, thr_id))
                                {
                                        applog(LOG_INFO,
                                               "GPU #%d: candidate %08x dropped, the job changed "
                                               "while the batch was running",
                                               dev_id, candidate[c]);
                                        continue;
                                }

                                if (fulltest(vhash, ptarget))
                                {
                                        /* submit_solution reads the nonce from
                                         * pdata[19], and the GPU's integer is
                                         * already in that orientation. */
                                        pdata[19] = candidate[c];
                                        submit_solution(work, vhash, mythr);
                                }
                                else
                                {
                                        /* Which candidate, and by how much,
                                         * because the three causes look alike
                                         * in a bare message and are not
                                         * equally benign:
                                         *
                                         *  - a new job arriving mid-batch, so
                                         *    the header checked here is not
                                         *    the one the GPU hashed. Harmless,
                                         *    and refusing to submit is right.
                                         *  - the device comparing only the top
                                         *    64 bits of the hash, so a near
                                         *    miss can reach here. Also
                                         *    harmless.
                                         *  - the kernels genuinely disagreeing
                                         *    with the CPU, which is a bug and
                                         *    would show as a top word far
                                         *    above target rather than just
                                         *    over it.
                                         */
                                        /* endiandata[19] is printed because the
                                         * failures repeat one hash value across
                                         * many different nonces. If the word
                                         * being hashed varies while the hash
                                         * does not, the fault is inside
                                         * x16r_hash; if the word itself is
                                         * stuck, it is in this function. */
                                        /* The rate is a uniform 22% and tracks
                                         * neither the first nor the last
                                         * algorithm in the chain, so guessing
                                         * has run out. Instead of asserting
                                         * which transform is wrong, try the
                                         * plausible ones and let the miner say
                                         * which, if any, would have validated:
                                         * the nonce written raw rather than
                                         * byte swapped, the batch base added or
                                         * removed twice, or a small
                                         * neighbourhood in case the reported
                                         * index is simply off. */
                                        /* Which side is wrong? Hash the exact
                                         * same bytes again. A different answer
                                         * means this verification is not
                                         * deterministic and the fault is here,
                                         * not on the card. The same answer
                                         * means the GPU really did compute
                                         * something else. Everything tried so
                                         * far has failed to tell those apart.
                                         */
                                        uint32_t again[8];
                                        int stable;

                                        x16r_prehash(endiandata, pdata, x16r_hash_order);
                                        stable = x16r_hash(again, endiandata, thr_id)
                                                 && memcmp(again, vhash, 32) == 0;

                                        /* The exact 80 bytes that disagreed,
                                         * so they can be put to Avian Core's
                                         * own implementation afterwards. Our
                                         * CPU matches Core across the seeds
                                         * the sweep covers, but these failures
                                         * span orders the sweep never reached,
                                         * and a proxy is not the same as the
                                         * case in hand. Printed before the
                                         * window sweep, which overwrites
                                         * word 19. */
                                        {
                                                char hex[161];
                                                uint32_t hdr[20];
                                                memcpy(hdr, endiandata, 80);
                                                be32enc(&hdr[19], candidate[c]);
                                                for (int b = 0; b < 80; b++)
                                                        sprintf(hex + b * 2, "%02x",
                                                                ((const uint8_t *)hdr)[b]);
                                                hex[160] = 0;
                                                applog(LOG_WARNING,
                                                       "GPU #%d:   header %s", dev_id, hex);
                                        }

                                        /* Is the hash wrong, or is the nonce
                                         * wrong by more than the one or two
                                         * already ruled out? Sweep a window
                                         * around the reported nonce and report
                                         * the offset of any nonce that does
                                         * meet the target. A consistent offset
                                         * means the thread-to-nonce mapping is
                                         * off and the kernels are fine; no hit
                                         * anywhere in the window means the GPU
                                         * saw a hash that does not exist.
                                         *
                                         * 8192 hashes at roughly a megahash a
                                         * second is a few milliseconds, which
                                         * only has to be paid on a failure.
                                         */
                                        const int WINDOW = 4096;
                                        long delta = LONG_MIN;
                                        /* Captured before the scan below,
                                         * which leaves endiandata[19] at
                                         * candidate + WINDOW. Printing it
                                         * afterwards reported a nonce 4096
                                         * away from the one being judged and
                                         * cost an hour chasing a byte order
                                         * that was never wrong. */
                                        const uint32_t w19 = endiandata[19];

                                        for (int d = -WINDOW; d <= WINDOW && delta == LONG_MIN; d++)
                                        {
                                                uint32_t t[8];
                                                be32enc(&endiandata[19], (uint32_t)(candidate[c] + d));
                                                x16r_prehash(endiandata, pdata, x16r_hash_order);
                                                if (x16r_hash(t, endiandata, thr_id)
                                                    && fulltest(t, ptarget))
                                                        delta = d;
                                        }

                                        applog(LOG_WARNING,
                                               "GPU #%d: %s nonce %08x (word19 %08x, start %08x, "
                                               "order %s) did not validate (hash[7] %08x vs "
                                               "target[7] %08x) cpu=%s window=%s",
                                               dev_id, c == 0 ? "first" : "second",
                                               candidate[c], w19, start, hashOrder,
                                               vhash[7], ptarget[7],
                                               stable ? "stable" : "UNSTABLE",
                                               delta == LONG_MIN ? "miss" : "HIT");
                                        if (delta != LONG_MIN)
                                                applog(LOG_WARNING,
                                                       "GPU #%d:   a valid nonce sits at offset %+ld "
                                                       "from the one reported",
                                                       dev_id, delta);
                                }
                        }

                        cudaDeviceSynchronize();
                        /* scanhash submits and reports progress; cpu-miner.c
                         * treats any non-zero return as a bug. pdata[19] is
                         * left at the first nonce not yet scanned so the next
                         * call resumes there. */
                        pdata[19] = start + throughput;
                        *hashes_done = pdata[19] - first_nonce;
                        return 0;
                }
'''
driver = driver[:i] + replacement + driver[j:]

# --- 5. Nothing else may still reach ccminer-only host state.
# work->nonces[1] is deliberately kept: it is cleared alongside the device
# copy. What must not survive is upstream's submission convention, which
# returned a count for the caller to act on, and its own CPU hash.
for banned in ["work_set_target_ratio", "bn_hash_target_ratio",
               "return res;", "x16rt_hash("]:
    if banned in driver:
        raise SystemExit("still references %r after patching" % banned)

# Exactly the one reset, and nothing reading it back.
n = driver.count("work->nonces[1]")
if n != 1:
    raise SystemExit("expected 1 use of work->nonces[1] (the reset), found %d" % n)

header = r'''/* X16RT on the GPU.
 *
 * The kernel dispatch below is suprminer-jdd's scanhash_x16rt, copied
 * verbatim from commit e4efcea so that it cannot drift: a transcription slip
 * in a sixteen-deep chain of hash functions is a wrong hash rather than a
 * build error. Generated by scripts/gen-driver.py; see cuda/README.md.
 *
 * What is ours is the boundary. cpuminer-opt's scanhash submits its own
 * solutions and returns zero, where ccminer's returned a count for the caller
 * to submit, and the result is checked against this miner's own CPU X16RT
 * rather than the second sphlib implementation upstream carried. That check
 * matters: the CPU path is what --avian-selftest and
 * avian/compare-with-avian-pow.sh already hold against Avian Core, so it is
 * the one implementation here already known to agree with consensus.
 */

#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <limits.h>

#include "cuda/avian-cuda-shim.h"
#include "cuda/avian-cuda-device.h"
#include "cuda/upstream/x16r/cuda_x16r.h"
#include "algo/x16/x16rt-cuda.h"

/* Declared in the CPU implementation. Sized X16R_HASH_FUNC_COUNT + 1 there;
 * x16rt-cuda-gate.c includes both headers so a disagreement is a compile
 * error rather than a buffer overrun. */
extern "C" __thread char x16r_hash_order[17];

/* Where a chain the GPU gets wrong is sent instead. Set by
 * register_x16rt_cuda_algo from whatever CPU scanhash this build selected. */
static x16rt_scanhash_fn cpu_fallback = NULL;

extern "C" void x16rt_cuda_set_cpu_fallback( x16rt_scanhash_fn fn )
{
        cpu_fallback = fn;
}

/* ccminer prefixed its log lines with the device number. applog has no such
 * notion, so this keeps the upstream call sites unchanged. */
#define gpulog(prio, dev, fmt, ...) \
        applog(prio, "GPU #%d: " fmt, (int) (dev), ##__VA_ARGS__)

/* cuda_x16_whirlpool.cu builds its first block on the host under
 * HOST_MIDSTATE and expects this. Upstream defined it in x16r.cu with its own
 * sphlib; this uses the one already in the tree, whose context has the same
 * eight-word state at the same offset. */
extern "C" {
#include "algo/whirlpool/sph_whirlpool.h"
}

/* Not extern "C": cuda_x16_whirlpool.cu declares it as a plain extern, so the
 * definition has to carry C++ linkage to match. */
void whirlpool_midstate(void *state, const void *input)
{
        sph_whirlpool_context ctx;

        sph_whirlpool_init(&ctx);
        sph_whirlpool(&ctx, input, 64);
        memcpy(state, ctx.state, 64);
}

'''

out = (header
       + "/* ---- upstream x16r.cu:37-64, device buffers and kernel prototypes ---- */\n"
       + rng(37, 64) + "\n\n"
       + "/* ---- upstream x16r.cu:66-105, the function ordering ---- */\n"
       + rng(66, 105) + "\n\n"
       + "/* ---- upstream x16r.cu:106-108 ---- */\n"
       + rng(106, 108) + "\n\n"
       + lines[269] + "\n\n"
       + "/* ---- upstream x16r.cu:1121-1757, patched at the boundary ---- */\n"
       + driver + "\n")

io.open(OUT, "w", encoding="utf-8", errors="surrogateescape", newline="").write(out)
print("wrote %s: %d lines" % (OUT, out.count("\n")))

# The stage bisect lives in its own script so the driver and the diagnostic can
# be read apart from each other. It is inert unless AVIAN_GPU_TRACE_STAGES is
# set at run time, so it stays in every build rather than being a special one.
_tracer = os.path.join(os.path.dirname(os.path.abspath(__file__)), "add-tracer.py")
if os.path.exists(_tracer):
    import subprocess
    if subprocess.run([sys.executable, _tracer, OUT]).returncode != 0:
        raise SystemExit("add-tracer.py failed; the driver would build without the bisect")
else:
    print("  add-tracer.py not beside the generator; no stage bisect in this build")

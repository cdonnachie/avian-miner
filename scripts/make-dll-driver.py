#!/usr/bin/env python3
"""Turn the generated CUDA driver into avian_cuda_scan_batch.

The Windows GPU build needs the kernels in a DLL, because nvcc will only use
MSVC as its host compiler while the miner and its Rust Stratum v2 library are
MinGW. That makes the seam a real ABI, so it carries plain data only: no
struct work, whose layout would have to agree across two compilers, and no
thread-local state, because a DLL's TLS is a separate block from the exe's.

So the kernels find candidates and the miner verifies and submits them. Linux
links the same function, so there is one implementation exercised by every
test rather than a Windows-only path nothing covers. That matters here
specifically: today's defect lived in a path the selftest could not reach.

Every substitution asserts its anchor. A silent miss would produce a driver
that builds and mines the wrong thing, which is the failure mode this project
has already paid for twice.
"""
import io
import re
import sys

PROLOGUE_OLD = '''extern "C" int scanhash_x16rt_cuda( struct work *work, uint32_t max_nonce,
                                    uint64_t *hashes_done,
                                    struct thr_info *mythr )
{
        const int thr_id = mythr->id;
        uint32_t *pdata = work->data;
        uint32_t *ptarget = work->target;
        const uint32_t first_nonce = pdata[19];
        const int dev_id = device_map[thr_id];
'''

PROLOGUE_NEW = '''/* One batch of nonces. See avian-cuda-api.h for why the seam is here. */
extern "C" AVIAN_CUDA_API int avian_cuda_scan_batch( const avian_cuda_job *job,
                                                     uint32_t *out_nonces,
                                                     int max_out,
                                                     uint32_t *throughput_used )
{
        const int thr_id = job->thr_id;
        /* Copied rather than aliased: the kernels only read the header, and a
         * local copy means nothing here can scribble on the miner's work. */
        uint32_t _ALIGN(64) pdata_copy[20];
        uint32_t _ALIGN(64) ptarget_copy[8];
        uint32_t *pdata = pdata_copy;
        uint32_t *ptarget = ptarget_copy;
        uint32_t nonce0 = UINT32_MAX, nonce1 = UINT32_MAX;
        const int dev_id = device_map[thr_id];
        uint64_t hashes_done_local = 0;
        uint64_t *hashes_done = &hashes_done_local;

        /* A CUDA error is sticky until something reads it, and almost
         * nothing in the vendored kernels does. The one exception is
         * skein512_cpu_setBlock_80, which calls cudaGetLastError() and
         * aborts the miner on whatever it finds, so an error left by an
         * earlier batch killed the process the next time SKEIN happened to
         * lead the chain. Read it here, report it, and start clean, so the
         * error is attributed to the batch that caused it rather than to an
         * unrelated kernel one epoch in sixteen later. */
        {
                cudaError_t stale = cudaGetLastError();
                if (stale != cudaSuccess)
                        applog(LOG_ERR, "GPU #%d: a previous batch left \\"%s\\" "
                                        "unreported", dev_id,
                                        cudaGetErrorString(stale));
        }

        memcpy(pdata_copy, job->data, sizeof(pdata_copy));
        memcpy(ptarget_copy, job->target, sizeof(ptarget_copy));
        pdata[19] = job->start_nonce;
        const uint32_t first_nonce = pdata[19];
        const uint32_t max_nonce = job->max_nonce;
'''

ORDER_OLD = '''        if ( s_ntime  != masked_ntime )
        {
           x16rt_getTimeHash( masked_ntime, &timeHash );
           x16rt_getAlgoString( &timeHash[0], hashOrder );
           s_ntime = masked_ntime;
           if ( !(thr_id || opt_quiet) )
           {
              /* endiandata[17] is already the byte-swapped nTime that the
               * CPU path passes here. */
              x16rt_log_hash_order( hashOrder, endiandata[17] );
              cudaDeviceSynchronize();
           }
        }
'''

ORDER_NEW = '''        /* The miner derives the chain and hands it over. Deriving it here as
         * well would put a consensus-critical rule on both sides of an ABI,
         * where the two could disagree and only one of them is tested. */
        (void)masked_ntime;
        (void)timeHash;
        memcpy(hashOrder, job->order, 17);
        hashOrder[16] = '\\0';
        if (s_ntime != masked_ntime) {
                s_ntime = masked_ntime;
        }
'''

DIAG_DECLS = '''/* Diagnostics only. Left unset by the miner, the stage bisect turns itself
 * off rather than comparing against nothing and reporting agreement. */
static avian_cuda_hash_n_fn  avian_diag_hash_n  = NULL;
static avian_cuda_prehash_fn avian_diag_prehash = NULL;

extern "C" AVIAN_CUDA_API void avian_cuda_set_diagnostics(avian_cuda_hash_n_fn hash_n,
                                                          avian_cuda_prehash_fn prehash)
{
        avian_diag_hash_n  = hash_n;
        avian_diag_prehash = prehash;
}

'''

TAIL_ANCHOR = "                        uint32_t _ALIGN(64) vhash[8];"

TAIL_NEW = '''                        /* The miner verifies and submits. A candidate is
                         * only the device's opinion; what stands between a
                         * kernel defect and a bad share is the CPU hash, and
                         * that lives on the miner's side of this boundary. */
                        int found = 0;
                        if (found < max_out) out_nonces[found++] = nonce0;

                        /* A batch can hold a second solution, reported one of
                         * two ways depending on whether the last stage ran a
                         * _final kernel. Kept exactly as the original driver
                         * had it: the two paths count nonces in different
                         * frames, so conflating them would offer a nonce from
                         * the wrong place in the batch. */
                        if (addstart) {
                                uint32_t second = h_resNonce[thr_id][1];
                                if (second != UINT32_MAX && found < max_out) {
                                        out_nonces[found++] = second + start;
                                }
                        } else {
                                uint32_t second = cuda_check_hash_suppl(thr_id, throughput,
                                                                        start, d_hash[thr_id], 1);
                                if (second != 0 && second != UINT32_MAX && found < max_out) {
                                        out_nonces[found++] = second;
                                }
                        }
                        if (throughput_used) *throughput_used = throughput;
                        avian_batch_end(dev_id);
                        return found;
                }
                if (throughput_used) *throughput_used = throughput;
                avian_batch_end(dev_id);
                return 0;
        }
        return 0;
}
'''

BATCH_END = '''
/* Synchronise and say whether the batch left anything behind. Naming the
 * batch that caused an error is the whole point: without this the error
 * waits, silent, until skein512_cpu_setBlock_80 reads it and the miner dies
 * in a kernel that had nothing to do with it. */
static void avian_batch_end(int dev_id)
{
        cudaError_t err = cudaDeviceSynchronize();

        if (err == cudaSuccess)
                err = cudaGetLastError();
        if (err != cudaSuccess)
                applog(LOG_ERR, "GPU #%d: the batch ended with \\"%s\\"",
                       dev_id, cudaGetErrorString(err));
}

'''


def sub_once(s, old, new, what):
    n = s.count(old)
    if n != 1:
        raise SystemExit("  ERROR: %s matched %d times, expected 1" % (what, n))
    return s.replace(old, new, 1)


def main(src, dst):
    s = io.open(src, encoding="utf-8", errors="surrogateescape", newline="").read()

    s = sub_once(s, PROLOGUE_OLD, BATCH_END + PROLOGUE_NEW, "prologue")
    s = sub_once(s, ORDER_OLD, ORDER_NEW, "order derivation")

    # The CPU fallback is dead code now (its condition starts with 0) and it
    # called back into the miner, which this boundary does not allow.
    m = re.search(r"\n        if \(0 && algo80 == SIMD && cpu_fallback\).*?\n        \}\n",
                  s, re.S)
    if not m:
        raise SystemExit("  ERROR: cpu_fallback block not found")
    s = s[:m.start()] + "\n" + s[m.end():]

    # One batch per call: the miner owns the loop, because it is the miner
    # that knows whether a new job has landed.
    s = sub_once(s, "        do {\n", "        {\n", "batch loop open")
    s = sub_once(s,
                 "                pdata[19] += throughput;\n"
                 "                } while (!work_restart[thr_id].restart && "
                 "((uint64_t)max_nonce > (uint64_t)throughput + pdata[19]));\n",
                 "                }\n", "batch loop close")

    # Candidate slots become locals; work is on the other side of the ABI.
    s = s.replace("work->nonces[0]", "nonce0").replace("work->nonces[1]", "nonce1")

    # Host globals become job fields.
    s = s.replace("opt_benchmark", "job->benchmark")
    s = s.replace("opt_quiet", "job->quiet")
    s = s.replace("opt_debug", "job->debug")
    s = re.sub(r"work_restart\[thr_id\]\.restart",
               "(job->restart && *job->restart)", s)

    # Replace the verify-and-submit tail.
    i = s.index(TAIL_ANCHOR)
    s = s[:i] + TAIL_NEW

    # The stage bisect compares the device against the miner's CPU chain, so
    # it is the one thing here that has to call back. It is diagnostics, not
    # consensus, so it goes through pointers the miner may leave unset, and
    # disables itself when they are. Consensus verification never comes this
    # way: that stays on the miner's side.
    if "avian_trace_compare" in s:
        s = sub_once(s, "        memcpy(x16r_hash_order, order, 17);\n", "",
                     "tracer hash-order copy")
        s = sub_once(s,
                     "                x16r_prehash(hdr, pdata, x16r_hash_order);",
                     "                avian_diag_prehash(hdr, pdata, order);",
                     "tracer prehash call")
        s = sub_once(s,
                     "                if (!x16r_hash_generic(cpu, hdr, thr_id, order, stages))",
                     "                if (!avian_diag_hash_n(cpu, hdr, thr_id, order, stages))",
                     "tracer hash call")
        s = sub_once(s, "#define AVIAN_TRACE_SAMPLES 256",
                     DIAG_DECLS + "#define AVIAN_TRACE_SAMPLES 256",
                     "diagnostics declarations")
        # Without a verifier there is nothing to compare against, so say so
        # once rather than silently reporting everything as agreeing.
        s = sub_once(s,
                     '                if (cached)\n'
                     '                        applog(LOG_NOTICE, "GPU: profiling every stage of one batch, "',
                     '                if (cached && !avian_diag_hash_n)\n'
                     '                {\n'
                     '                        applog(LOG_ERR, "GPU: the stage profile needs the miner\'s "\n'
                     '                                        "verifier, which this build did not hand over");\n'
                     '                        cached = 0;\n'
                     '                }\n'
                     '                if (cached)\n'
                     '                        applog(LOG_NOTICE, "GPU: profiling every stage of one batch, "',
                     "diagnostics availability guard")

    # Asserted like every other substitution. An unasserted replace is how
    # this one silently did nothing the first time: the include is spelled
    # "cuda/avian-cuda-shim.h", and the file then failed to compile on a
    # macro it had never been given.
    s = sub_once(s, '#include "cuda/avian-cuda-shim.h"',
                 '#include "cuda/avian-cuda-shim.h"\n#include "cuda/avian-cuda-api.h"',
                 "api header include")

    io.open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)

    leaks = [n for n in ("submit_solution", "fulltest", "x16r_hash(",
                         "struct work", "mythr", "x16rt_getAlgoString")
             if n in s]
    print("  wrote %s: %d lines" % (dst, s.count("\n")))
    if leaks:
        print("  WARNING: still references %s" % ", ".join(leaks))
        return 1
    print("  no miner-side symbols remain in the batch function")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))

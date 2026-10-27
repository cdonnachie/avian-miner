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
        avian_phase_check(dev_id, "a previous batch");

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
        /* Diagnostics: see avian_force_order. */
        {
                const char *forced = avian_force_order();
                if (forced) memcpy(hashOrder, forced, 16);
        }
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
/* Read the pending CUDA error, say which phase of the batch left it, and
 * then shut up about it.
 *
 * A CUDA error is sticky until something reads it, and almost nothing in
 * the vendored kernels does. The one exception is skein512_cpu_setBlock_80,
 * which calls cudaGetLastError() and aborts on whatever it finds, so an
 * error left by any batch killed the miner the next time SKEIN happened to
 * lead the chain. Reading it here keeps that from happening and names the
 * phase responsible.
 *
 * Capped deliberately. This fires every batch, which at a few hundred
 * batches a second buries every other line in the log and is worse than the
 * silence it replaced. */
static void avian_report(int dev_id, const char *phase, cudaError_t err)
{
        static int complained = 0;

        if (complained < 3)
        {
                applog(LOG_ERR, "GPU #%d: %s left \\"%s\\"",
                       dev_id, phase, cudaGetErrorString(err));
                if (++complained == 3)
                        applog(LOG_ERR, "GPU #%d: further CUDA errors will not "
                                        "be reported", dev_id);
        }
}

static void avian_phase_check(int dev_id, const char *phase)
{
        cudaError_t err = cudaGetLastError();

        if (err != cudaSuccess)
                avian_report(dev_id, phase, err);
}

/* Forces anything still in flight to complete before judging it. A peek
 * with cudaGetLastError() alone cannot see a failure that has not surfaced
 * yet, which is how a bad copy got attributed to a kernel that ran later. */
static void avian_sync_check(int dev_id, const char *phase)
{
        cudaError_t err = cudaDeviceSynchronize();

        if (err == cudaSuccess)
                err = cudaGetLastError();
        if (err != cudaSuccess)
                avian_report(dev_id, phase, err);
}

static void avian_batch_end(int dev_id)
{
        /* Capture the synchronise's own return. A kernel launch failure is
         * asynchronous: it is not visible at the launch, and it is
         * cudaDeviceSynchronize that reports it. Ignoring this return was
         * why the phase checks all came back clean while the error was
         * plainly reaching skein512_cpu_setBlock_80 later. */
        cudaError_t err = cudaDeviceSynchronize();

        if (err != cudaSuccess)
                avian_report(dev_id, "the batch's kernels", err);
        else
                avian_phase_check(dev_id, "the candidate search");
}

/* With AVIAN_GPU_SYNC_EACH_STAGE set, synchronise after every stage so an
 * asynchronous launch failure is attributed to the kernel that caused it
 * rather than to whatever happens to synchronise next. Far too slow to mine
 * with, which is why it is off unless asked for. */
static int avian_sync_each_stage(void)
{
        static int cached = -1;

        if (cached < 0)
        {
                const char *e = getenv("AVIAN_GPU_SYNC_EACH_STAGE");
                cached = (e && *e && *e != '0') ? 1 : 0;
        }
        return cached;
}

static void avian_stage_check(int dev_id, int stage, const char *name)
{
        cudaError_t err;

        if (!avian_sync_each_stage())
                return;
        err = cudaDeviceSynchronize();
        if (err == cudaSuccess)
                err = cudaGetLastError();
        if (err != cudaSuccess)
        {
                char what[64];
                snprintf(what, sizeof(what), "stage %d (%s)", stage, name);
                avian_report(dev_id, what, err);
        }
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

    # cudaDeviceReset() destroys the primary context, and with it every
    # module and __constant__ symbol registration made before it, including
    # the ones avian_cuda_device_count() set up when it took the device
    # inventory. Afterwards cudaMemcpyToSymbol can fail with "invalid
    # argument", and almost nothing in the vendored kernels reads a CUDA
    # error, so it sat unnoticed until skein512_cpu_setBlock_80, the one
    # entry point that calls cudaGetLastError(), aborted the miner one epoch
    # in sixteen. Resetting the device is an application's business, not a
    # library's, and this code is a library now.
    old_reset = """                if (opt_cudaschedule == -1 && gpu_threads == 1)
                {
                        cudaDeviceReset();
                        // reduce cpu usage
                        cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
                }
"""
    new_reset = """                if (opt_cudaschedule == -1 && gpu_threads == 1)
                {
                        /* No cudaDeviceReset() here: see make-dll-driver.py.
                         * It would tear down the context this library's own
                         * caller already set up. */
                        cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
                }
"""
    s = sub_once(s, old_reset, new_reset, "device reset")

    # The CPU fallback is dead code now (its condition starts with 0) and it
    # called back into the miner, which this boundary does not allow.
    m = re.search(r"\n        if \(0 && algo80 == SIMD && cpu_fallback\).*?\n        \}\n",
                  s, re.S)
    if not m:
        raise SystemExit("  ERROR: cpu_fallback block not found")
    s = s[:m.start()] + "\n" + s[m.end():]

    # One batch per call: the miner owns the loop, because it is the miner
    # that knows whether a new job has landed.
    # Three checks, so the log says which phase of the batch is at fault
    # rather than only that something is. Device setup and buffer allocation
    # happen once, the chain runs every batch, and the candidate search is
    # last, so between them they cover everything a batch does.
    s = sub_once(s, "        do {\n",
                 "        {\n"
                 "                avian_phase_check(dev_id, \"device setup\");\n",
                 "batch loop open")
    # The 80-byte setBlock switch runs before the batch loop, so a check
    # placed inside the loop is already too late: skein512_cpu_setBlock_80
    # reads the error first and aborts. This one sits between the target
    # upload and that switch, which is the last gap left.
    # Synchronise, do not merely peek. cudaMemcpyToSymbol to a __constant__
    # can return before it has completed, so a plain cudaGetLastError() one
    # line later reports success and the failure surfaces at whatever calls
    # cudaGetLastError() next, which is skein512_cpu_setBlock_80 one epoch
    # in sixteen. That is the whole reason this took so long to find.
    s = sub_once(s, "        cuda_check_cpu_setTarget(ptarget);\n",
                 "        cuda_check_cpu_setTarget(ptarget);\n"
                 "        avian_sync_check(dev_id, \"the target upload\");\n",
                 "target upload check")

    # A per-stage synchronise, so an asynchronous launch failure is pinned
    # to the kernel that caused it. Placed at the top of the loop body, so
    # the check at index i covers the stage that index i - 1 launched.
    s = sub_once(s,
                 "                        elem = hashOrder[i];\n",
                 "                        avian_stage_check(dev_id, i, hashOrder);\n"
                 "                        elem = hashOrder[i];\n",
                 "per-stage sync check")

    chain_end = "\n                if (!addstart)\n                {\n"
    s = sub_once(s, chain_end,
                 "\n                avian_phase_check(dev_id, \"the hash chain\");\n"
                 + chain_end, "chain-end check")
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

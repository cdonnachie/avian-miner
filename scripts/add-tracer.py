#!/usr/bin/env python3
"""Add diagnostics to the generated CUDA driver. All are inert unless their
environment variable is set, so they live in every build.

AVIAN_GPU_TRACE_ALL=1   (needs AVIAN_GPU_NO_FINAL=1)
    Profile every stage of one batch, then stop. This is the one to use.
    Sweeping AVIAN_GPU_TRACE_STAGES across separate runs cannot localise a
    stage, because each run lands in a different 128-second X16RT epoch and
    so measures a different chain order. Comparing stage 10 of one order
    against stage 11 of another says nothing. Profiling one batch gives every
    stage for the same order.

AVIAN_GPU_TRACE_STAGES=K  (1..16)
    Stop after K kernels and compare only that stage. Still useful to confirm
    a single stage cheaply once TRACE_ALL has named it.

Both compare the device's d_hash against x16r_hash_generic truncated to the
same number of stages, for the nonces the device used, and both sample across
the whole batch rather than from its start. The first version of this sampled
only threads 0-63 of 524288 and reported stages 1-14 bit-exact; the threads
that actually diverge turned out to be 63488, 67584, 73728 and 79872, so that
result was an artefact of where it looked.

Stage 16 needs AVIAN_GPU_NO_FINAL=1, because the _final kernel does not write
its hash back, so otherwise d_hash is a stage short and every sample differs
for an uninteresting reason.

AVIAN_GPU_NO_FINAL=1
    Run the plain kernel for the last stage too and find candidates by
    scanning d_hash, swapping out the _final kernel path. It announces itself
    on first use, because an A/B whose switch silently did nothing is worse
    than no A/B at all. Measured 2026-10-27: 19.7% against 24.3%, so that
    path is not the fault.
"""
import io
import sys

SAMPLES = 256

NO_FINAL_HELPER = r'''
static int avian_no_final(void)
{
        static int cached = -1;
        if (cached < 0)
        {
                const char *e = getenv("AVIAN_GPU_NO_FINAL");
                cached = (e && *e && *e != '0') ? 1 : 0;
                if (cached)
                        applog(LOG_NOTICE, "GPU: AVIAN_GPU_NO_FINAL is set; the last "
                                           "stage runs the plain kernel and candidates "
                                           "come from a d_hash scan");
        }
        return cached;
}

'''

TRACE_HELPER = r'''
#define AVIAN_TRACE_SAMPLES 256

static int avian_trace_all(void)
{
        static int cached = -1;
        if (cached < 0)
        {
                const char *e = getenv("AVIAN_GPU_TRACE_ALL");
                cached = (e && *e && *e != '0') ? 1 : 0;
                if (cached && !avian_no_final())
                {
                        applog(LOG_ERR, "GPU: AVIAN_GPU_TRACE_ALL needs "
                                        "AVIAN_GPU_NO_FINAL=1, so the last stage's "
                                        "hash reaches d_hash");
                        cached = 0;
                }
                if (cached)
                        applog(LOG_NOTICE, "GPU: profiling every stage of one batch, "
                                           "up to %d threads sampled",
                                           AVIAN_TRACE_SAMPLES);
        }
        return cached;
}

static int avian_trace_stages(void)
{
        static int cached = -2;
        if (cached == -2)
        {
                const char *e = getenv("AVIAN_GPU_TRACE_STAGES");
                cached = (e && *e) ? atoi(e) : 0;
                if (cached < 1 || cached > 16) cached = 0;
                if (cached == 16 && !avian_no_final())
                {
                        applog(LOG_ERR, "GPU: AVIAN_GPU_TRACE_STAGES=16 needs "
                                        "AVIAN_GPU_NO_FINAL=1, or d_hash is a stage "
                                        "short and every sample differs for nothing");
                        cached = 0;
                }
                if (cached)
                        applog(LOG_NOTICE, "GPU: stage bisect on, stopping after %d "
                                           "kernel(s), up to %d threads sampled per "
                                           "batch", cached, AVIAN_TRACE_SAMPLES);
        }
        return cached;
}

/* Compares the device's d_hash against the CPU chain truncated to the same
 * number of stages, for the nonces the device actually used. Thread t held
 * nonce start + t, because the 80-byte kernel is launched with startNonce =
 * pdata[19] and start is a copy of it taken in the same batch.
 *
 * Returns the number of sampled threads that differed, or -1 if the sample
 * had to be abandoned. */
static int avian_trace_compare(int thr_id, int dev_id, int stages,
                               uint32_t *d_hash_p, uint32_t *endiandata,
                               uint32_t *pdata, const char *order,
                               uint32_t start, uint32_t threads, int quiet)
{
        uint32_t hdr[20], cpu[16], gpu[16], cpu_first[16], gpu_first[16];
        uint32_t stride = threads / AVIAN_TRACE_SAMPLES;
        uint32_t first_t = 0;
        int bad = 0, first = -1, taken = 0;

        if (stride == 0) stride = 1;
        /* x16r_hash_generic takes the order as an argument, but x16r_prehash
         * reads the global the normal verification path uses, so the two
         * cannot disagree here in a way they would not in production. */
        memcpy(x16r_hash_order, order, 17);
        memcpy(hdr, endiandata, 80);

        for (int j = 0; j < AVIAN_TRACE_SAMPLES; j++)
        {
                /* Last sample is pinned to the final thread, so the end of
                 * the batch is always covered however the stride divides. */
                uint32_t t = (j == AVIAN_TRACE_SAMPLES - 1)
                             ? threads - 1 : (uint32_t)j * stride;
                if (t >= threads) break;

                if (cudaMemcpy(gpu, d_hash_p + (size_t)t * 16, 64,
                               cudaMemcpyDeviceToHost) != cudaSuccess)
                {
                        applog(LOG_WARNING, "GPU #%d: trace copy failed", dev_id);
                        return -1;
                }
                be32enc(&hdr[19], start + t);
                x16r_prehash(hdr, pdata, x16r_hash_order);
                /* Returns 0 without writing when a new job lands mid-chain.
                 * Comparing then would be noise, so abandon the sample. */
                if (!x16r_hash_generic(cpu, hdr, thr_id, order, stages))
                        return -1;
                taken++;
                if (memcmp(cpu, gpu, 64) != 0)
                {
                        if (first < 0)
                        {
                                first = j;
                                first_t = t;
                                memcpy(cpu_first, cpu, 64);
                                memcpy(gpu_first, gpu, 64);
                        }
                        bad++;
                }
        }

        if (!quiet || bad)
                applog(bad ? LOG_WARNING : LOG_INFO,
                       "GPU #%d: TRACE stage %2d of %s: %3d/%d differ (stride %u of %u)",
                       dev_id, stages, order, bad, taken, stride, threads);
        if (first >= 0 && !quiet)
                applog(LOG_WARNING,
                       "GPU #%d:   first at thread %u (nonce %08x) gpu %08x%08x "
                       "cpu %08x%08x",
                       dev_id, first_t, start + first_t,
                       gpu_first[0], gpu_first[1], cpu_first[0], cpu_first[1]);
        return bad;
}

'''

TRACE_CALL = '''                        /* At loop index i, exactly i kernels have run. In the
                         * all-stage profile the comparison is taken and the
                         * chain carries on, so one batch yields every stage
                         * for one chain order. In single-stage mode we return
                         * rather than break, to keep a partial chain out of
                         * the candidate path. */
                        if (avian_trace_all())
                                avian_trace_compare(thr_id, dev_id, i, d_hash[thr_id],
                                                    endiandata, pdata, hashOrder, start,
                                                    throughput, 0);
                        else if (avian_trace_stages() == i)
                        {
                                avian_trace_compare(thr_id, dev_id, i, d_hash[thr_id],
                                                    endiandata, pdata, hashOrder, start,
                                                    throughput, 0);
                                *hashes_done = 0;
                                return 0;
                        }
'''

TRACE_CALL_16 = '''                /* Stage 16, the complete chain. Only reachable with
                 * AVIAN_GPU_NO_FINAL=1, which is what leaves the last stage's
                 * hash in d_hash for this to read. */
                if (avian_trace_all() || avian_trace_stages() == 16)
                {
                        avian_trace_compare(thr_id, dev_id, 16, d_hash[thr_id],
                                            endiandata, pdata, hashOrder, start,
                                            throughput, 0);
                        *hashes_done = 0;
                        return 0;
                }

'''

SIG = 'extern "C" int scanhash_x16rt_cuda( struct work *work, uint32_t max_nonce,'

LOOP = ("                for (int i = 1; i < 16; i++)\n"
        "                {\n"
        "                        elem = hashOrder[i];\n")

LOOP_NEW = ("                for (int i = 1; i < 16; i++)\n"
            "                {\n"
            + TRACE_CALL +
            "                        elem = hashOrder[i];\n")

POST = "\n                if (!addstart)\n                {\n"
POST_NEW = "\n" + TRACE_CALL_16 + "                if (!addstart)\n                {\n"

FINAL_SPELLINGS = ("if (i == 15)", "if(i== 15)")
FINAL_SITES = 14


def main(path):
    s = io.open(path, encoding="utf-8", newline="").read()
    if "avian_trace_compare" in s:
        print("  diagnostics already present")
        return 0

    for name, anchor in (("scanhash signature", SIG), ("chain loop", LOOP),
                         ("post-loop", POST)):
        if s.count(anchor) != 1:
            print("  ERROR: %s anchor matched %d times" % (name, s.count(anchor)))
            return 1

    s = s.replace(SIG, NO_FINAL_HELPER + TRACE_HELPER + SIG, 1)
    s = s.replace(LOOP, LOOP_NEW, 1)
    s = s.replace(POST, POST_NEW, 1)

    n = 0
    for spelling in FINAL_SPELLINGS:
        n += s.count(spelling)
        s = s.replace(spelling, "if (i == 15 && !avian_no_final())")
    if n != FINAL_SITES:
        print("  ERROR: expected %d final-kernel sites, found %d" % (FINAL_SITES, n))
        return 1

    io.open(path, "w", encoding="utf-8", newline="").write(s)
    print("  stage profile added, %d samples per batch, %d final-kernel sites switchable"
          % (SAMPLES, n))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

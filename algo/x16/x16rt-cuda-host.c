/* The miner's half of the CUDA X16RT backend.
 *
 * The kernels find candidate nonces; this verifies and submits them. The
 * split exists because Windows needs the kernels in a DLL (nvcc will only
 * use MSVC as its host compiler, while the miner and its Rust Stratum v2
 * library are MinGW), and a DLL boundary cannot carry struct work or
 * thread-local state. See cuda/avian-cuda-api.h.
 *
 * Linux links avian_cuda_scan_batch directly rather than keeping a separate
 * in-process driver, so there is one implementation that every test
 * exercises. That is deliberate: the defect found on 2026-10-27 lived in a
 * path the selftest could not reach, and a Windows-only code path would be
 * the same mistake in a new place.
 *
 * Verification stays here, on the miner's side, for the same reason. A
 * candidate is only the device's opinion. The CPU hash is what stands
 * between a kernel defect and a bad share, and it is the scalar path that
 * --avian-selftest now covers.
 */

#include <stdint.h>
#include <string.h>

#include "algo/x16/x16r-gate.h"
#include "algo/x16/x16rt-cuda.h"
#include "cuda/avian-cuda-api.h"
#include "miner.h"

/* Two is what a batch can report: the winning nonce and one runner-up. */
#define AVIAN_MAX_CANDIDATES 4

int scanhash_x16rt_cuda( struct work *work, uint32_t max_nonce,
                         uint64_t *hashes_done, struct thr_info *mythr )
{
    uint32_t _ALIGN(64) edata[20];
    uint32_t _ALIGN(64) timeHash[8];
    uint32_t _ALIGN(64) vhash[8];
    uint32_t candidates[AVIAN_MAX_CANDIDATES];
    uint32_t *pdata = work->data;
    uint32_t *ptarget = work->target;
    const uint32_t first_nonce = pdata[19];
    const int thr_id = mythr->id;
    avian_cuda_job job;
    static __thread uint32_t s_ntime = UINT32_MAX;

    if ( opt_benchmark )
    {
        ptarget[7] = 0x0ff;
        pdata[1] = 0x88888888;
        pdata[2] = 0x88888888;
    }

    /* The chain is derived here and handed over, rather than on both sides of
     * the boundary where the two could disagree and only one is tested. */
    for ( int k = 0; k < 19; k++ )
        be32enc( &edata[k], pdata[k] );

    const uint32_t masked_ntime = edata[17] & 0xffffff80;
    if ( s_ntime != masked_ntime )
    {
        x16rt_getTimeHash( masked_ntime, &timeHash );
        x16rt_getAlgoString( &timeHash[0], x16r_hash_order );
        s_ntime = masked_ntime;
        if ( !( thr_id || opt_quiet ) )
            x16rt_log_hash_order( x16r_hash_order, edata[17] );
    }

    memset( &job, 0, sizeof(job) );
    memcpy( job.data, pdata, sizeof(job.data) );
    memcpy( job.order, x16r_hash_order, 17 );
    job.thr_id    = thr_id;
    job.max_nonce = max_nonce;
    job.benchmark = opt_benchmark;
    job.quiet     = opt_quiet;
    job.debug     = opt_debug;
    /* Read through the pointer, not copied: the miner sets this while a
     * batch is running, which is the whole point of it. */
    job.restart   = (const volatile unsigned char *) &work_restart[thr_id].restart;

    uint32_t throughput = 0;
    do
    {
        job.start_nonce = pdata[19];
        /* Refreshed every batch, not snapshotted once before the loop.
         * mining.set_difficulty can arrive without a new job, so the target
         * tightens under a scan that is already running. Taken once, the
         * device goes on selecting candidates against the old looser target
         * while the check below judges them against the new one, and every
         * candidate in the batch is reported as a disagreement. Nothing
         * invalid can be submitted, because that check is what gates
         * submission, but the batch is wasted and the log is alarming. It
         * shows up reliably on connect, where a default target is replaced
         * moments later, and on any vardiff pool afterwards. */
        memcpy( job.target, ptarget, sizeof(job.target) );

        int found = avian_cuda_scan_batch( &job, candidates,
                                           AVIAN_MAX_CANDIDATES, &throughput );
        if ( found < 0 )
        {
            applog( LOG_ERR, "GPU #%d: the batch failed (%d)", thr_id, found );
            *hashes_done = pdata[19] - first_nonce;
            return 0;
        }

        for ( int c = 0; c < found; c++ )
        {
            if ( c && candidates[c] == candidates[0] )
                continue;

            /* edata holds the header as big-endian words, and be32enc of the
             * device's nonce leaves word 19 in the orientation x16r_hash
             * expects: ccminer and this miner describe the same convention
             * with the nonce byte-swapped between them, so the two cancel. */
            be32enc( &edata[19], candidates[c] );
            x16r_prehash( edata, pdata, x16r_hash_order );

            /* Returns 0 without writing when a new job lands mid-batch.
             * Treating that as a disagreement once reported uninitialised
             * stack as a hash, so the candidate is simply dropped: it
             * belongs to the previous job and is worthless either way. */
            if ( !x16r_hash( vhash, edata, thr_id ) )
                continue;

            if ( fulltest( vhash, ptarget ) )
            {
                pdata[19] = candidates[c];
                submit_solution( work, vhash, mythr );
            }
            else
            {
                /* Caught before submission, so no invalid share is ever sent;
                 * it costs throughput only. The stage bisect in
                 * avian-cuda-api.h is the instrument if this ever returns. */
                applog( LOG_WARNING,
                        "GPU #%d: nonce %08x did not validate (hash[7] %08x vs "
                        "target[7] %08x, order %s)",
                        thr_id, candidates[c], vhash[7], ptarget[7],
                        x16r_hash_order );
            }
        }

        pdata[19] = job.start_nonce + throughput;

    } while ( !work_restart[thr_id].restart
              && ( (uint64_t) max_nonce > (uint64_t) throughput + pdata[19] ) );

    *hashes_done = pdata[19] - first_nonce;
    return 0;
}

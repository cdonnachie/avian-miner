/* The miner's half of the CUDA X16RT backend.
 *
 * The kernels find candidate nonces; this verifies and submits them. The
 * split exists because Windows needs the kernels in a DLL (nvcc will only
 * use MSVC as its host compiler, while the miner and its Rust Stratum v2
 * library are MinGW), and a DLL boundary cannot carry struct work or
 * thread-local state. See cuda/avian-cuda-api.h.
 *
 * Linux loads the same library through the same entry point rather than
 * keeping a separate in-process driver, so there is one implementation that
 * every test exercises. That is deliberate: the defect found on 2026-10-27
 * lived in a path the selftest could not reach, and a Windows-only code path
 * would be the same mistake in a new place.
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
#include "cuda/avian-gpu-backend.h"
#include "miner.h"

/* Two is what a batch can report: the winning nonce and one runner-up. */
#define AVIAN_MAX_CANDIDATES 4

/* How many batches in a row may fail before this stops trying.
 *
 * A card that fails repeatedly is almost always one that has left the PCIe
 * bus, usually a riser, and nothing the miner can do re-enumerates it.
 * Carrying on means an error line per iteration and no hashes, so a rig
 * quietly loses that card's share of its rate while still looking busy.
 *
 * Small, because there is nothing to wait for. A real transient, a job
 * arriving mid-batch, does not arrive here: the backend reports zero nonces
 * tried and the loop below handles that separately. */
#define AVIAN_GPU_MAX_FAILURES 5

/* One slot per possible device rather than per mining thread, so --devices
 * cannot shift the indices under it. Consecutive failures only: one good
 * batch clears a card's count, because this is for a card that has gone and
 * not for one that hiccups. */
#define AVIAN_GPU_MAX_TRACKED 32
static int s_batch_failures[ AVIAN_GPU_MAX_TRACKED ] = { 0 };

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

    /* Hand the backend the miner's own CPU chain, once, so the stage bisect
     * has something to compare the device against. Nothing in the normal
     * mining path goes through these: consensus verification is x16r_hash
     * further down, on this side of the boundary, and stays there.
     *
     * This call was missing, and its absence was invisible on the development
     * machine, because the binary used for verification there had been built
     * from a working tree that carried it uncommitted. So the diagnostic that
     * proves the device agrees with the CPU could not be reproduced from a
     * clean checkout, which is the one property such a diagnostic has to
     * have. On Linux it did worse than not work: AVIAN_GPU_TRACE_STAGES
     * called through the null pointer and dumped core. */
    static bool s_diag_wired = false;
    if ( !s_diag_wired )
    {
        s_diag_wired = true;
        avian_gpu_set_diagnostics( x16r_hash_generic, x16r_prehash );
    }

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

        /* thr_id is also the device, because the gate set one thread per
         * device over the whole list. The loader turns it into the owning
         * backend and that backend's own index, which are not the same
         * number once both vendors are present. */
        int found = avian_gpu_scan_batch( thr_id, &job, candidates,
                                          AVIAN_MAX_CANDIDATES, &throughput );
        int slot = ( thr_id >= 0 && thr_id < AVIAN_GPU_MAX_TRACKED )
                   ? thr_id : 0;

        if ( found < 0 )
        {
            applog( LOG_ERR, "GPU #%d: the batch failed (%d)", thr_id, found );

            if ( ++s_batch_failures[ slot ] >= AVIAN_GPU_MAX_FAILURES )
            {
                /* Exiting rather than carrying on, because a supervisor
                 * restart is the one thing that does recover a card which
                 * has left the bus, and every mining distribution restarts
                 * a miner that exits. A bare shell does not, so the message
                 * says what happened and what to go and look at.
                 *
                 * hard_exit, not proper_exit: exit() would run this very
                 * driver's teardown against the device that just stopped
                 * answering, which is how the miner became unkillable in
                 * the first place. */
                applog( LOG_ERR, "GPU #%d: %d batches failed in a row, so "
                                 "this card is gone rather than busy. "
                                 "Exiting so the rig restarts us, which is "
                                 "what re-enumerates it. Check the riser and "
                                 "the PCIe seating if it does not come back.",
                        thr_id, AVIAN_GPU_MAX_FAILURES );
                hard_exit( 1 );
            }
            *hashes_done = pdata[19] - first_nonce;
            return 0;
        }
        s_batch_failures[ slot ] = 0;

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
                /* Every CPU scanhash in the tree guards its submit with
                 * !bench and this one did not. The target set above lets
                 * nearly every hash through, so a benchmark submitted
                 * several fake shares a second to a pool it does not have:
                 * a failed HTTP request each time, a retry loop in the
                 * workio thread that sleeps between attempts, and, because
                 * solo mining forces fresh work after a block, a
                 * restart_threads that ended each scan at its first
                 * candidate rather than at the end of its window.
                 *
                 * Measured either way the reported rate is the same within
                 * noise, so this is about the log and the scan window, not
                 * throughput. The verify above still runs, because its cost
                 * belongs in the figure. */
                if ( !opt_benchmark )
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

        /* A backend that tried nothing cannot advance the nonce, so the
         * condition below would be unchanged and this would spin for ever on
         * empty batches. Both backends have a floor that stops them reporting
         * zero, and both grew it after spinning, so the loop now refuses to
         * depend on that. */
        if ( throughput == 0 )
        {
            applog( LOG_ERR, "GPU #%d: the batch tried no nonces", thr_id );
            break;
        }

    } while ( !work_restart[thr_id].restart
              && ( (uint64_t) max_nonce > (uint64_t) throughput + pdata[19] ) );

    *hashes_done = pdata[19] - first_nonce;
    return 0;
}

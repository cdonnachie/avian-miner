/* How far a miner may roll nTime, and why it is not simply the job's window.
 *
 * On its own so it can be tested without a miner, a pool or a node: see
 * avian/ntime-test.c. Nothing in here touches the library handle or the miner,
 * which is the point.
 */

#include "avian/ntime.h"

uint32_t avian_sv2_next_ntime(const AvianMiningJob *job, uint32_t current)
{
    uint32_t limit = job->ntime_max;

    if (job->pow_type == AVIAN_SV2_POW_X16RT) {
        /* The second bound, and the one that matters. X16RT derives its hash
         * chain from SHA256d(nTime & 0xffffff80), so a nTime one second past
         * the end of the job's 128-second epoch is a different hash function.
         * The share would not be wrong-looking, it would be computed against
         * the wrong problem, and nothing in the protocol would say so. A
         * pool's window may be wider than an epoch; the epoch still wins. */
        uint32_t epoch_end = (job->ntime & AVIAN_X16RT_EPOCH_MASK)
                           + AVIAN_X16RT_EPOCH_SECONDS - 1;
        if (epoch_end < limit) {
            limit = epoch_end;
        }
    }

    if (current < job->ntime_min) {
        /* Below the window: start at its bottom, if that is inside the limit
         * at all. */
        return job->ntime_min <= limit ? job->ntime_min : 0;
    }
    if (current >= limit) {
        return 0;
    }
    return current + 1;
}

uint32_t avian_sv2_ntime_limit(const AvianMiningJob *job)
{
    uint32_t limit = job->ntime_max;

    if (job->pow_type == AVIAN_SV2_POW_X16RT) {
        uint32_t epoch_end = (job->ntime & AVIAN_X16RT_EPOCH_MASK)
                           + AVIAN_X16RT_EPOCH_SECONDS - 1;
        if (epoch_end < limit) {
            limit = epoch_end;
        }
    }
    return limit;
}

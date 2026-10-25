/* How far a miner may roll nTime.
 *
 * Two bounds, and the tighter one wins. AvianJobTimeConstraints gives
 * [ntime_min, ntime_max], and a share outside it is one the pool will refuse.
 * X16RT adds a second bound the protocol cannot express: its hash chain comes
 * from SHA256d(nTime & 0xffffff80), so rolling past a 128-second boundary
 * silently computes a different function. A miner that did so would not be
 * mining badly, it would be mining a different problem.
 *
 * This header pulls in nothing but the library's job type, so the rule can be
 * tested on its own.
 */

#ifndef AVIAN_NTIME_H
#define AVIAN_NTIME_H

#include <stdint.h>

#include "avian_sv2.h"

/* Where an X16RT epoch starts and how long it lasts. The mask is the one
 * Avian's own x16rt_seed applies. */
#define AVIAN_X16RT_EPOCH_MASK 0xffffff80u
#define AVIAN_X16RT_EPOCH_SECONDS 128u

/* The next nTime to try after `current`, or 0 when there is none.
 *
 * `current` below the job's window snaps to ntime_min. Returning 0 means stop
 * and wait for another job rather than roll anyway. */
uint32_t avian_sv2_next_ntime(const AvianMiningJob *job, uint32_t current);

/* The highest nTime this job may be hashed at: its ntime_max, or the end of
 * its epoch for X16RT, whichever is lower. */
uint32_t avian_sv2_ntime_limit(const AvianMiningJob *job);

#endif /* AVIAN_NTIME_H */

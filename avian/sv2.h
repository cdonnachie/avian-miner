/* Stratum v2 for Avian, alongside this miner's Stratum v1.
 *
 * Nothing here replaces a v1 code path. have_sv2 is a third source of work
 * next to have_stratum and getwork, and the only edits in cpu-miner.c are
 * branches that test it.
 *
 * The hashing code is untouched, deliberately. scanhash_minotaur expects
 * work->data in Stratum v1's convention, big-endian 32-bit words, and
 * byteswaps it back before hashing. This backend fills work->data in that
 * same convention, so scanhash and minotaur_hash do not know Stratum v2
 * exists.
 *
 * Licence: GNU GPL v2, as the rest of this program. libavian-sv2 is MIT and
 * every crate it pulls in offers MIT or CC0, so linking it into a GPLv2
 * binary is clean.
 */

#ifndef AVIAN_SV2_BACKEND_H
#define AVIAN_SV2_BACKEND_H

#include <stdbool.h>

#include "miner.h"
#include "algo-gate-api.h"

/* g_work is written by whichever source of work is active. */
extern struct work g_work;

/* Set from the command line. */
extern char *opt_sv2_url;
extern char *opt_sv2_authority;
extern char *opt_sv2_user;
extern char *opt_sv2_expect_payout;
extern bool opt_sv2_standard;
extern int opt_sv2_extranonce_size;

/* Connects, opens a channel and keeps g_work current. Signature matches
 * thread_create. Never returns while the connection can be re-established. */
void *avian_sv2_thread(void *userdata);

/* Submits a share found for this work. Returns false if the job it was found
 * for is no longer one the backend holds, which is a stale share rather than
 * an error. */
bool avian_sv2_submit(struct work *work);

/* Advances the current job's nTime by one second, if the job's window and
 * its algorithm both allow it, and restarts the miner threads on the fresh
 * nonce range that buys. False means there is nothing left to roll and the
 * only thing to do is wait for another job, which is the answer on every
 * chain a developer can run: nBits is a function of nTime there, so the node
 * pins ntime_min == ntime_max. See avian/ntime.h for the two bounds. */
bool avian_sv2_roll_ntime(void);

/* Checks this build's proof of work, for whichever of x16rt and minotaurx
 * opt_algo names, against hashes a real Avian node produced. Returns 0 when
 * they agree. */
int avian_pow_selftest(void);

/* What this machine does on the configured algorithm, in hashes a second,
 * measured by running it for `seconds` on every mining thread. Zero if it
 * cannot be measured, which leaves the caller to fall back.
 *
 * Must not be called before work_restart is allocated: scanhash reads its
 * own slot. */
double avian_sv2_measure_hash_rate( double seconds );

/* submit_solution's first call. False in every ordinary run, so nothing about
 * mining changes; the self-test uses it to take the hash the miner computed
 * without a share going anywhere. */
bool avian_selftest_capture(const void *hash);

#endif /* AVIAN_SV2_BACKEND_H */

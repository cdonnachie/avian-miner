/* The nTime rolling rule, asserted without a miner, a pool or a node.
 *
 * It cannot be tested end to end on any chain that exists to test on.
 * fPowAllowMinDifficultyBlocks is true on regtest, testnet, testnet4 and
 * signet, and false only on mainnet, so everywhere a developer can run, nBits
 * is a function of nTime and the node pins ntime_min == ntime_max. There is no
 * window to roll in. That makes this the only place the rule is checked, which
 * is a reason to check it thoroughly rather than a reason to skip it.
 *
 * Build and run: avian/ntime-test.sh
 */

#include <stdio.h>
#include <string.h>

#include "avian/ntime.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static AvianMiningJob make_job(int32_t pow_type, uint32_t ntime, uint32_t min,
                               uint32_t max)
{
    AvianMiningJob job;
    memset(&job, 0, sizeof(job));
    job.pow_type = pow_type;
    job.ntime = ntime;
    job.ntime_min = min;
    job.ntime_max = max;
    return job;
}

/* Walks the roll from the job's start and reports where it got to and how many
 * steps it took. */
static uint32_t walk(const AvianMiningJob *job, int *steps, int *left_epoch)
{
    uint32_t at = job->ntime;
    uint32_t epoch = job->ntime & AVIAN_X16RT_EPOCH_MASK;
    uint32_t next;
    int n = 0;

    *left_epoch = 0;
    while ((next = avian_sv2_next_ntime(job, at)) != 0) {
        at = next;
        n++;
        if ((at & AVIAN_X16RT_EPOCH_MASK) != epoch) {
            *left_epoch = 1;
        }
        if (n > 4096) {
            /* A rule that never says stop is the bug this guards against. */
            break;
        }
    }
    *steps = n;
    return at;
}

int main(void)
{
    /* A boundary to sit on, and a time a little way inside an epoch. */
    const uint32_t boundary = 1792968320u; /* & 0xffffff80 == itself */
    int steps, left;
    uint32_t end;

    printf("nTime rolling\n\n");

    printf("the window, with no epochs in the way (minotaurx)\n");
    {
        AvianMiningJob job =
            make_job(AVIAN_SV2_POW_MINOTAURX, 1000, 1000, 1005);
        check(avian_sv2_next_ntime(&job, 1000) == 1001, "rolls forward by one");
        check(avian_sv2_next_ntime(&job, 1004) == 1005, "reaches ntime_max");
        check(avian_sv2_next_ntime(&job, 1005) == 0, "stops at ntime_max");
        check(avian_sv2_next_ntime(&job, 1006) == 0, "never returns past it");
        end = walk(&job, &steps, &left);
        check(end == 1005 && steps == 5, "walks the window and no further");
    }

    printf("a window pinned shut, which is every chain a developer can run\n");
    {
        AvianMiningJob job = make_job(AVIAN_SV2_POW_X16RT, boundary, boundary,
                                      boundary);
        check(avian_sv2_next_ntime(&job, boundary) == 0,
              "ntime_min == ntime_max leaves nothing to roll");
        end = walk(&job, &steps, &left);
        check(steps == 0 && end == boundary, "hashes only the job's own ntime");
    }

    printf("below the window\n");
    {
        AvianMiningJob job = make_job(AVIAN_SV2_POW_MINOTAURX, 500, 1000, 1005);
        check(avian_sv2_next_ntime(&job, 500) == 1000,
              "snaps up to ntime_min rather than rolling from below it");
        /* A job whose window starts in the epoch *after* its own nTime. A
         * pool should not send that, which is exactly why the answer has to
         * be no rather than a hash against the wrong chain. */
        AvianMiningJob unreachable =
            make_job(AVIAN_SV2_POW_X16RT, boundary, boundary + 200,
                     boundary + 300);
        check(avian_sv2_next_ntime(&unreachable, 0) == 0,
              "says no when the window starts past the job's epoch");
        check(avian_sv2_next_ntime(&unreachable, boundary) == 0,
              "and says no from the job's own ntime too");
    }

    printf("x16rt stops at the epoch boundary even when the pool allows more\n");
    {
        /* A window 10 minutes wide, far beyond one epoch. */
        AvianMiningJob job = make_job(AVIAN_SV2_POW_X16RT, boundary, boundary,
                                      boundary + 600);
        check(avian_sv2_ntime_limit(&job) == boundary + 127,
              "the limit is the end of the epoch, not ntime_max");
        check(avian_sv2_next_ntime(&job, boundary + 126) == boundary + 127,
              "reaches the last second of the epoch");
        check(avian_sv2_next_ntime(&job, boundary + 127) == 0,
              "will not take the step across the boundary");
        end = walk(&job, &steps, &left);
        check(!left, "never leaves the job's epoch");
        check(end == boundary + 127 && steps == 127,
              "uses the whole epoch and exactly the whole epoch");
    }

    printf("a job that starts mid-epoch gets what is left of it, not 128\n");
    {
        AvianMiningJob job = make_job(AVIAN_SV2_POW_X16RT, boundary + 100,
                                      boundary + 100, boundary + 600);
        check(avian_sv2_ntime_limit(&job) == boundary + 127,
              "the limit is still that epoch's end");
        end = walk(&job, &steps, &left);
        check(!left && steps == 27, "rolls the 27 seconds it actually has");
    }

    printf("and the same window does cross a boundary for minotaurx\n");
    {
        /* The clamp has to be the algorithm's, not an accident of arithmetic
         * that would also throttle MinotaurX for no reason. */
        AvianMiningJob job = make_job(AVIAN_SV2_POW_MINOTAURX, boundary,
                                      boundary, boundary + 600);
        check(avian_sv2_ntime_limit(&job) == boundary + 600,
              "MinotaurX is bounded only by the pool's window");
        end = walk(&job, &steps, &left);
        check(left, "crosses 128-second boundaries freely, having no epochs");
        check(end == boundary + 600 && steps == 600, "uses the whole window");
    }

    printf("\n");
    if (failures) {
        printf("FAIL: %d assertion(s)\n", failures);
        return 1;
    }
    printf("OK: nTime rolling respects the window and, for X16RT, the epoch\n");
    return 0;
}

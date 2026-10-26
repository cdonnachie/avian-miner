/* The Stratum v2 backend. See avian/sv2.h for why it is a third work source
 * rather than a replacement for either existing one. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "avian/ntime.h"
#include "avian/sv2.h"

#include "avian_sv2.h"

char *opt_sv2_url = NULL;
char *opt_sv2_authority = NULL;
char *opt_sv2_user = NULL;
char *opt_sv2_expect_payout = NULL;
bool opt_sv2_standard = false;
int opt_sv2_extranonce_size = 0;

/* Jobs this backend still accepts a share for.
 *
 * A Stratum v2 job id is only unique within a connection, so a share carries
 * a token from here instead. A reconnect, or simply eight newer jobs, retires
 * a slot and the share is refused as stale rather than being applied to
 * whatever job now holds that id. */
#define JOB_SLOTS 8
#define MAX_EXTRANONCE 32

struct job_slot {
    AvianMiningJob job;
    uint8_t extranonce[MAX_EXTRANONCE];
    size_t extranonce_len;
    uint64_t token;
};

/* Shares waiting for the backend thread. The library handle is not
 * synchronised and must be used from one thread, so miner threads queue
 * rather than calling into it. */
#define SUBMIT_SLOTS 64

struct submission {
    uint64_t token;
    uint32_t nonce;
    uint32_t ntime;
};

static struct job_slot g_jobs[JOB_SLOTS];
static uint64_t g_next_token = 1;
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;

static struct submission g_queue[SUBMIT_SLOTS];
static int g_queue_head = 0;
static int g_queue_count = 0;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;

static AvianSv2Client *g_client = NULL;

/* The rolling extranonce. An extended channel lets this backend choose it, and
 * it has to differ between jobs or two jobs with the same prev_hash would be
 * the same search. Threads do not need their own: std_get_new_work already
 * gives each one its own slice of the nonce. */
static uint64_t g_extranonce_counter = 0;

/* Which algorithm to ask the pool for. -a selects it, the same flag that
 * selects the hasher, so the two cannot disagree. */
static int32_t avian_sv2_pow_type(void)
{
    return opt_algo == ALGO_X16RT ? AVIAN_SV2_POW_X16RT
                                  : AVIAN_SV2_POW_MINOTAURX;
}

static struct job_slot *find_job(uint64_t token)
{
    for (int i = 0; i < JOB_SLOTS; i++) {
        if (g_jobs[i].token == token) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

static void forget_all_jobs(void)
{
    pthread_mutex_lock(&g_jobs_lock);
    memset(g_jobs, 0, sizeof(g_jobs));
    pthread_mutex_unlock(&g_jobs_lock);
}

/* work->data in Stratum v1's convention: each 32-bit word byte-reversed
 * against the header, because scanhash reverses it back before hashing. */
static void header_to_work_data(const uint8_t *header, uint32_t *data)
{
    for (int i = 0; i < 20; i++) {
        data[i] = ((uint32_t)header[i * 4 + 0] << 24)
                | ((uint32_t)header[i * 4 + 1] << 16)
                | ((uint32_t)header[i * 4 + 2] << 8)
                | ((uint32_t)header[i * 4 + 3]);
    }
}

/* The block height, from the BIP34 push that opens the coinbase scriptSig.
 *
 * Only on an extended channel, because that is where the miner is given the
 * coinbase at all; a standard channel sends a finished merkle root and
 * nothing to parse. Without this every submission printed "Block 0", and on a
 * solo pool, where the whole proposition is that consensus pays the miner for
 * blocks it finds, that is the one number a user looks for. */
static int height_from_coinbase(const uint8_t *coinbase, size_t len)
{
    /* version(4) + input count(1) + prevout(36) + script length(1), then the
     * scriptSig, which opens with the height push. */
    const size_t script_at = 4 + 1 + 36 + 1;
    if (len < script_at + 1) {
        return 0;
    }
    size_t push = coinbase[script_at];
    /* A push of more than four bytes is not a height, and a height needs at
     * least one. Either way, better to say nothing than to print a number
     * read out of the wrong field. */
    if (push < 1 || push > 4 || len < script_at + 1 + push) {
        return 0;
    }
    uint32_t height = 0;
    for (size_t i = 0; i < push; i++) {
        height |= (uint32_t)coinbase[script_at + 1 + i] << (i * 8);
    }
    return (int)height;
}

/* What the pool committed to pay, read out of the coinbase the miner itself
 * assembled. This is the whole point of an extended channel: on a standard one
 * the pool hands over a finished merkle root and the miner can only take its
 * word for what is in the block. */
static void check_payout(const AvianMiningJob *job, const uint8_t *extranonce,
                         size_t extranonce_len)
{
    static bool reported = false;
    unsigned char want[64];
    size_t want_len;
    uint8_t *coinbase;
    size_t needed = 0;
    size_t written = 0;

    if (reported || !opt_sv2_expect_payout || !job->extended) {
        if (!reported && opt_sv2_expect_payout && !job->extended) {
            applog(LOG_WARNING, "SV2: cannot verify the payout on a standard "
                                "channel; the pool holds the coinbase");
            reported = true;
        }
        return;
    }

    /* pk_buffer_size, not sizeof(want): address_to_script uses its size
     * argument as the base58 decode length as well as the output capacity, so
     * anything but 25 (or 26 for sapling) makes the checksum fail and the
     * address silently look invalid. cpu-miner.c says as much where
     * pk_buffer_size is declared. */
    want_len = address_to_script(want, pk_buffer_size, opt_sv2_expect_payout);
    if (want_len == 0 || want_len > sizeof(want)) {
        applog(LOG_ERR, "SV2: cannot turn %s into a script, so the payout "
                        "cannot be verified", opt_sv2_expect_payout);
        reported = true;
        return;
    }

    /* The sizing call's status is ignored on purpose. The header documents
     * calling with capacity 0 to size a buffer, but that path returns
     * AVIAN_SV2_INVALID_ARGUMENT because the capacity is too small, and sets
     * no error message. *written is still filled in, so that is what to
     * believe. */
    avian_sv2_client_job_coinbase(g_client, job->job_id, extranonce,
                                  extranonce_len, NULL, 0, &needed);
    if (needed == 0) {
        applog(LOG_ERR, "SV2: the job has no coinbase to read: %s",
               avian_sv2_last_error(g_client));
        return;
    }
    coinbase = (uint8_t *)malloc(needed);
    if (!coinbase) {
        return;
    }
    if (avian_sv2_client_job_coinbase(g_client, job->job_id, extranonce,
                                      extranonce_len, coinbase, needed,
                                      &written)
        != AVIAN_SV2_OK) {
        applog(LOG_ERR, "SV2: could not read the coinbase: %s",
               avian_sv2_last_error(g_client));
        free(coinbase);
        return;
    }

    /* A substring search is enough here. The script is 25 bytes of a specific
     * shape and the coinbase is a few hundred bytes of mostly a scriptSig, so
     * a false positive would need the pool to have embedded the exact script
     * somewhere it is not paying it, which is not a failure mode worth
     * parsing the whole transaction for. */
    bool found = false;
    if (written >= want_len) {
        for (size_t i = 0; i + want_len <= written; i++) {
            if (memcmp(coinbase + i, want, want_len) == 0) {
                found = true;
                break;
            }
        }
    }
    if (found) {
        applog(LOG_NOTICE, "SV2: the coinbase pays %s, verified from the "
                           "coinbase this miner assembled",
               opt_sv2_expect_payout);
    } else {
        applog(LOG_ERR, "SV2: the coinbase does NOT pay %s",
               opt_sv2_expect_payout);
    }
    reported = true;
    free(coinbase);
}

/* Turns a job into g_work and wakes the miner threads. */
static bool install_job(const AvianMiningJob *job)
{
    uint8_t extranonce[MAX_EXTRANONCE];
    size_t extranonce_len = 0;
    uint8_t header[80];
    struct job_slot *slot;
    uint64_t token;
    char token_hex[32];

    memset(extranonce, 0, sizeof(extranonce));
    if (job->extended && job->extranonce_size > 0) {
        extranonce_len = job->extranonce_size;
        if (extranonce_len > MAX_EXTRANONCE) {
            extranonce_len = MAX_EXTRANONCE;
        }
        uint64_t counter = ++g_extranonce_counter;
        for (size_t i = 0; i < extranonce_len && i < sizeof(counter); i++) {
            extranonce[i] = (uint8_t)(counter >> (i * 8));
        }
    }

    /* Works for both kinds of channel: a standard one ignores the extranonce
     * and uses the pool's merkle root, an extended one assembles the coinbase
     * and folds the path. */
    if (avian_sv2_client_job_header(g_client, job->job_id, extranonce,
                                    extranonce_len, job->ntime, 0, header)
        != AVIAN_SV2_OK) {
        applog(LOG_ERR, "SV2: could not build a header for job %u: %s",
               job->job_id, avian_sv2_last_error(g_client));
        return false;
    }

    check_payout(job, extranonce, extranonce_len);

    int height = 0;
    if (job->extended) {
        size_t needed = 0;
        avian_sv2_client_job_coinbase(g_client, job->job_id, extranonce,
                                      extranonce_len, NULL, 0, &needed);
        if (needed > 0) {
            uint8_t *coinbase = (uint8_t *)malloc(needed);
            if (coinbase != NULL) {
                size_t written = 0;
                if (avian_sv2_client_job_coinbase(g_client, job->job_id, extranonce,
                                                  extranonce_len, coinbase, needed,
                                                  &written)
                    == AVIAN_SV2_OK) {
                    height = height_from_coinbase(coinbase, written);
                }
                free(coinbase);
            }
        }
    }

    pthread_mutex_lock(&g_jobs_lock);
    token = g_next_token++;
    slot = &g_jobs[token % JOB_SLOTS];
    slot->job = *job;
    memcpy(slot->extranonce, extranonce, sizeof(extranonce));
    slot->extranonce_len = extranonce_len;
    slot->token = token;
    pthread_mutex_unlock(&g_jobs_lock);

    snprintf(token_hex, sizeof(token_hex), "%" PRIx64, token);

    pthread_rwlock_wrlock(&g_work_lock);
    work_free(&g_work);
    memset(&g_work, 0, sizeof(g_work));
    header_to_work_data(header, g_work.data);
    /* share_target and work->target are both uint256's internal order, which
     * on a little-endian host is the same bytes.
     *
     * Copied, not derived from a difficulty, and that is load-bearing rather
     * than a shortcut. Stratum v1 fills the same field in stratum_gen_work by
     * dividing the pool's advertised difficulty by opt_target_factor, and
     * register_x16rt_algo sets that factor to 256 because Veil's X16RT uses
     * Veil's difficulty convention. An Avian miner reaching the target that
     * way would be hashing against one 256 times too easy, and the symptom
     * would be a flood of shares the pool rejects rather than an error.
     * Taking the pool's target as the bytes it sent cannot drift. */
    memcpy(g_work.target, job->share_target, 32);
    g_work.targetdiff = hash_to_diff(g_work.target);
    /* So a submission does not report "Block 0". Only an extended channel can
     * answer this, because only there does the miner hold the coinbase; on a
     * standard channel the pool sends a finished merkle root and the height is
     * not in anything the miner is given. Zero there is honest. */
    g_work.height = height;
    g_work.stratum_diff = g_work.targetdiff;
    g_work.job_id = strdup(token_hex);
    if (extranonce_len) {
        g_work.xnonce2 = (unsigned char *)malloc(extranonce_len);
        if (g_work.xnonce2) {
            memcpy(g_work.xnonce2, extranonce, extranonce_len);
            g_work.xnonce2_len = extranonce_len;
        }
    }
    g_work_time = time(NULL);
    pthread_rwlock_unlock(&g_work_lock);

    restart_threads();

    if (!opt_quiet) {
        applog(LOG_BLUE, "SV2 job %u, %s channel, diff %.5g%s", job->job_id,
               job->extended ? "extended" : "standard", g_work.targetdiff,
               job->ntime_min == job->ntime_max ? ", ntime pinned" : "");
    }
    return true;
}

bool avian_sv2_roll_ntime(void)
{
    AvianMiningJob job;
    struct job_slot *slot;
    uint64_t token;
    uint32_t current, next;

    pthread_rwlock_wrlock(&g_work_lock);
    if (!g_work.job_id) {
        pthread_rwlock_unlock(&g_work_lock);
        return false;
    }
    token = strtoull(g_work.job_id, NULL, 16);
    current = bswap_32(g_work.data[algo_gate.ntime_index]);

    pthread_mutex_lock(&g_jobs_lock);
    slot = find_job(token);
    if (slot) {
        job = slot->job;
    }
    pthread_mutex_unlock(&g_jobs_lock);
    if (!slot) {
        pthread_rwlock_unlock(&g_work_lock);
        return false;
    }

    next = avian_sv2_next_ntime(&job, current);
    if (next == 0) {
        pthread_rwlock_unlock(&g_work_lock);
        return false;
    }
    /* Only the header changes. nTime is not in the coinbase, so the
     * merkle root is the same on a standard channel and on an extended
     * one, and no library call is needed to rebuild anything. */
    g_work.data[algo_gate.ntime_index] = bswap_32(next);
    pthread_rwlock_unlock(&g_work_lock);

    if (opt_debug) {
        applog(LOG_DEBUG, "SV2: rolled ntime to %08x, limit %08x", next,
               avian_sv2_ntime_limit(&job));
    }
    restart_threads();
    return true;
}

bool avian_sv2_submit(struct work *work)
{
    uint64_t token;
    int index;

    if (!work->job_id) {
        return false;
    }
    token = strtoull(work->job_id, NULL, 16);

    pthread_mutex_lock(&g_jobs_lock);
    bool known = find_job(token) != NULL;
    pthread_mutex_unlock(&g_jobs_lock);
    if (!known) {
        applog(LOG_INFO, "SV2: share for a job the pool has retired, dropping");
        return false;
    }

    pthread_mutex_lock(&g_queue_lock);
    if (g_queue_count == SUBMIT_SLOTS) {
        pthread_mutex_unlock(&g_queue_lock);
        applog(LOG_WARNING, "SV2: submission queue full, dropping a share");
        return false;
    }
    index = (g_queue_head + g_queue_count) % SUBMIT_SLOTS;
    g_queue[index].token = token;
    /* work->data is byteswapped against the header, so undo that to recover
     * the values the protocol wants. */
    g_queue[index].nonce = bswap_32(work->data[algo_gate.nonce_index]);
    g_queue[index].ntime = bswap_32(work->data[algo_gate.ntime_index]);
    g_queue_count++;
    pthread_mutex_unlock(&g_queue_lock);
    return true;
}

static void drain_submissions(void)
{
    while (1) {
        struct submission item;
        struct job_slot slot;

        pthread_mutex_lock(&g_queue_lock);
        if (g_queue_count == 0) {
            pthread_mutex_unlock(&g_queue_lock);
            return;
        }
        item = g_queue[g_queue_head];
        g_queue_head = (g_queue_head + 1) % SUBMIT_SLOTS;
        g_queue_count--;
        pthread_mutex_unlock(&g_queue_lock);

        pthread_mutex_lock(&g_jobs_lock);
        struct job_slot *found = find_job(item.token);
        if (!found) {
            pthread_mutex_unlock(&g_jobs_lock);
            applog(LOG_INFO, "SV2: share went stale before it could be sent");
            continue;
        }
        slot = *found;
        pthread_mutex_unlock(&g_jobs_lock);

        int32_t status;
        if (slot.extranonce_len) {
            status = avian_sv2_client_submit_extended(
                g_client, &slot.job, item.nonce, item.ntime, slot.extranonce,
                slot.extranonce_len);
        } else {
            status = avian_sv2_client_submit(g_client, &slot.job, item.nonce,
                                             item.ntime);
        }
        if (status != AVIAN_SV2_OK) {
            applog(LOG_WARNING, "SV2: submit failed: %s",
                   avian_sv2_last_error(g_client));
        }
    }
}

static AvianSv2Client *connect_once(bool extended)
{
    AvianSv2ClientConfig config;
    int32_t status = 0;
    AvianSv2Client *client;

    memset(&config, 0, sizeof(config));
    config.pool = opt_sv2_url;
    config.authority_hex = opt_sv2_authority;
    config.pow_type = avian_sv2_pow_type();
    config.user_identity = opt_sv2_user;
    config.nominal_hash_rate = 1000.0f;
    config.extended = extended ? 1 : 0;
    config.min_extranonce_size = (uint32_t)opt_sv2_extranonce_size;
    /* A miner's whole job is to dial a pool somewhere else. The library
     * defaults to loopback so that a zeroed config cannot reach the network by
     * accident; a miner is the case that means to. */
    config.allow_public_pool = 1;

    client = avian_sv2_client_connect(&config, &status);
    if (!client) {
        applog(LOG_ERR, "SV2: %s channel refused: %s",
               extended ? "extended" : "standard",
               avian_sv2_last_connect_error());
    }
    return client;
}

void *avian_sv2_thread(void *userdata)
{
    struct thr_info *mythr = (struct thr_info *)userdata;

    if (avian_sv2_abi_check(sizeof(AvianMiningJob),
                            offsetof(AvianMiningJob, extranonce_size),
                            sizeof(AvianSv2Event),
                            offsetof(AvianSv2Event, message))
        != AVIAN_SV2_OK) {
        applog(LOG_ERR, "SV2: avian_sv2.h disagrees with libavian-sv2 about "
                        "struct layout; refusing to mine garbage");
        proper_exit(1);
    }

    if (opt_algo != ALGO_X16RT && opt_algo != ALGO_MINOTAURX) {
        applog(LOG_ERR, "SV2: Avian is x16rt and minotaurx; -a names "
                        "neither, so there is no pool to talk to");
        proper_exit(1);
    }
    applog(LOG_INFO, "SV2: libavian-sv2 %s, mining %s",
           avian_sv2_version(),
           opt_algo == ALGO_X16RT ? "x16rt" : "minotaurx");

    /* Extended first, because then the miner assembles its own coinbase and
     * can confirm what it is committing to. A pool that cannot spare the
     * extranonce says so, and a standard channel still mines. */
    if (!opt_sv2_standard) {
        g_client = connect_once(true);
        if (!g_client) {
            applog(LOG_WARNING, "SV2: falling back to a standard channel");
        }
    }
    if (!g_client) {
        g_client = connect_once(false);
    }
    if (!g_client) {
        applog(LOG_ERR, "SV2: could not open a channel on %s", opt_sv2_url);
        proper_exit(1);
    }

    while (1) {
        AvianSv2Event event;
        int32_t status;

        drain_submissions();

        memset(&event, 0, sizeof(event));
        status = avian_sv2_client_poll(g_client, 50, &event);
        if (status == AVIAN_SV2_ERROR || status == AVIAN_SV2_INVALID_ARGUMENT) {
            applog(LOG_ERR, "SV2: poll failed: %s",
                   avian_sv2_last_error(g_client));
            break;
        }

        switch (event.kind) {
        case AVIAN_SV2_EVENT_JOB:
            install_job(&event.job);
            break;
        case AVIAN_SV2_EVENT_JOBS_INVALIDATED:
            forget_all_jobs();
            applog(LOG_INFO, "SV2: the tip moved, previous jobs are void");
            restart_threads();
            break;
        case AVIAN_SV2_EVENT_SHARE_ACCEPTED:
            if (!opt_quiet) {
                applog(LOG_NOTICE, "SV2: share accepted (%u so far)",
                       event.accepted_count);
            }
            break;
        case AVIAN_SV2_EVENT_BLOCK_FOUND: {
            /* The pool telling the miner something the Mining Protocol has no
             * way to say. A miner is given a share target and never the
             * network target, so it cannot work this out for itself. */
            char printed[65];
            /* A block hash prints reversed. */
            for (int b = 0; b < 32; b++) {
                snprintf(printed + b * 2, 3, "%02x", event.block_hash[31 - b]);
            }
            if (event.block_confirmed) {
                applog(LOG_NOTICE,
                       "*** BLOCK CONFIRMED *** %s is the chain tip. "
                       "Share %u was yours and it paid you.",
                       printed, event.sequence_number);
            } else {
                /* Not "accepted". The pool has forwarded it and nothing has
                 * confirmed it yet, and no confirmation is unresolved rather
                 * than failure: a pool cannot tell a block that lost a race
                 * from one still propagating. */
                applog(LOG_NOTICE,
                       "*** BLOCK FOUND *** share %u produced %s, submitted to the "
                       "node; waiting to see it become the tip",
                       event.sequence_number, printed);
            }
            break;
        }
        case AVIAN_SV2_EVENT_SHARE_REJECTED:
            applog(LOG_WARNING, "SV2: share rejected: %s", event.message);
            break;
        case AVIAN_SV2_EVENT_RECONNECTED:
            /* Job ids start again on a new connection, so nothing held here
             * can be submitted against it. */
            forget_all_jobs();
            applog(LOG_INFO, "SV2: reconnected, waiting for a new job");
            restart_threads();
            break;
        case AVIAN_SV2_EVENT_DISCONNECTED:
            applog(LOG_WARNING, "SV2: disconnected: %s", event.message);
            break;
        default:
            break;
        }
    }

    tq_freeze(mythr->q);
    return NULL;
}

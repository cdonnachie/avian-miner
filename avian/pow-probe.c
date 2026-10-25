/* Prints this miner's proof-of-work hash for each 80-byte header given as hex.
 *
 *     pow-probe <x16rt|minotaurx> <header_hex>...
 *
 * It drives the miner's real scanhash rather than calling a hash function
 * directly, which matters for X16RT: the hash chain is derived from
 * SHA256d(nTime & 0xffffff80) inside scanhash, cached per thread, and fed to
 * x16r_prehash. A probe that reimplemented that would be testing the probe.
 * The target is set to all ones and valid_hash always says yes, so the first
 * nonce "wins" and submit_solution hands the hash back.
 *
 * Deliberately links nothing from Avian. avian-pow and this miner both vendor
 * yespower and export the same symbol names, so a single process containing
 * both resolves one implementation's call into the other and the comparison
 * proves nothing. Keeping each side in its own process is the only way the
 * answer means anything.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "algo-gate-api.h"
#include "algo/x16/x16r-gate.h"

/* Set by register_minotaur_algo, read by minotaur_hash. */
enum algos opt_algo = ALGO_MINOTAURX;
bool opt_benchmark = false;
algo_gate_t algo_gate;

bool register_minotaur_algo(algo_gate_t *gate);
/* Builds the thread-local torture garden's child pointers. The miner calls
 * this per thread through algo_gate.miner_thread_init; minotaur_hash walks
 * null pointers without it. X16RT sets no such hook. */
bool initialize_torture_garden(void);

/* Not a stub: cpuminer's yespower and scanhash_x16rt both read
 * work_restart[thr_id].restart while hashing, so this has to be real memory
 * and not a null pointer. */
static struct work_restart probe_restart[1];
struct work_restart *work_restart = probe_restart;

static uint8_t captured[32];
static int captured_any;

/* Always yes, so the first nonce reaches submit_solution and the hash comes
 * back out. Nothing here is deciding whether a share is worth anything. */
bool valid_hash(const void *hash, const void *target)
{
    (void)hash;
    (void)target;
    return true;
}

bool submit_solution(struct work *work, const void *hash, struct thr_info *thr)
{
    (void)work;
    (void)thr;
    /* The first one only. A wide scanhash computes eight nonces at once and,
     * with valid_hash always saying yes, submits all eight; lane 0 is
     * first_nonce, which is the one asked for. Setting restart stops the loop
     * after this iteration whatever its width. */
    if (!captured_any) {
        memcpy(captured, hash, sizeof(captured));
        captured_any = 1;
    }
    probe_restart[0].restart = 1;
    return true;
}

static int unhex(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) {
        return 0;
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned value;
        if (sscanf(hex + i * 2, "%2x", &value) != 1) {
            return 0;
        }
        out[i] = (uint8_t)value;
    }
    return 1;
}

static uint32_t be32_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    struct work work __attribute__((aligned(64)));
    struct thr_info thr;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <x16rt|minotaurx> <80-byte-header-hex>...\n",
                argv[0]);
        return 2;
    }

    memset(&algo_gate, 0, sizeof(algo_gate));
    if (strcmp(argv[1], "minotaurx") == 0) {
        opt_algo = ALGO_MINOTAURX;
        if (!register_minotaur_algo(&algo_gate)) {
            fprintf(stderr, "could not register minotaurx\n");
            return 2;
        }
    } else if (strcmp(argv[1], "x16rt") == 0) {
        opt_algo = ALGO_X16RT;
        if (!register_x16rt_algo(&algo_gate)) {
            fprintf(stderr, "could not register x16rt\n");
            return 2;
        }
    } else {
        fprintf(stderr, "unknown algorithm '%s'\n", argv[1]);
        return 2;
    }
    if (algo_gate.miner_thread_init && !algo_gate.miner_thread_init(0)) {
        fprintf(stderr, "miner_thread_init failed\n");
        return 2;
    }

    memset(&thr, 0, sizeof(thr));
    thr.id = 0;

    for (int i = 2; i < argc; i++) {
        uint8_t header[80];
        uint64_t hashes_done = 0;

        if (!unhex(argv[i], header, sizeof(header))) {
            fprintf(stderr, "argument %d is not 80 bytes of hex\n", i);
            return 2;
        }

        memset(&work, 0, sizeof(work));
        /* Stratum v1's convention, which is what scanhash expects: every word
         * byte-reversed against the header, because scanhash reverses it back
         * before hashing. The nonce is the exception: scanhash overwrites
         * edata[19] with pdata[19] as it stands, so that one goes in as the
         * header's own little-endian value. */
        for (int w = 0; w < 20; w++) {
            work.data[w] = be32_at(header + w * 4);
        }
        work.data[19] = le32_at(header + 76);
        memset(work.target, 0xff, sizeof(work.target));

        captured_any = 0;
        memset(captured, 0, sizeof(captured));
        probe_restart[0].restart = 0;
        /* Room for one pass of the widest scanhash. A wide one computes
         * last_nonce as max_nonce - 8, so max_nonce = first_nonce + 1 makes
         * that underflow and the loop never ends. */
        algo_gate.scanhash(&work, work.data[19] + 16, &hashes_done, &thr);
        if (!captured_any) {
            fprintf(stderr, "argument %d: the miner computed no hash\n", i);
            return 2;
        }
        for (size_t b = 0; b < sizeof(captured); b++) {
            printf("%02x", captured[b]);
        }
        printf("\n");
    }
    return 0;
}

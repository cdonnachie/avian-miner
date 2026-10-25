/* Prints cpuminer-opt's MinotaurX hash for each 80-byte header given as hex.
 *
 * Deliberately links nothing from Avian. avian-pow and cpuminer-opt both
 * vendor yespower and export the same symbol names, so a single process
 * containing both resolves one implementation's call into the other and the
 * comparison proves nothing. Keeping each side in its own process is the only
 * way the answer means anything.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "algo-gate-api.h"

int minotaur_hash(void *output, const void *input, int thr_id);
/* Builds the thread-local torture garden's child pointers. The miner calls
 * this per thread through algo_gate.miner_thread_init; minotaur_hash walks
 * null pointers without it. */
bool initialize_torture_garden(void);

/* minotaur.o's scanhash and gate registration share its translation unit. */
enum algos opt_algo = ALGO_MINOTAURX;
bool opt_benchmark = false;
algo_gate_t algo_gate;

bool submit_solution(struct work *work, const void *hash, struct thr_info *thr)
{
    (void)work;
    (void)hash;
    (void)thr;
    return false;
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

int main(int argc, char **argv)
{
    if (!initialize_torture_garden()) {
        fprintf(stderr, "initialize_torture_garden failed\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        uint8_t header[80];
        uint8_t hash[32];
        if (!unhex(argv[i], header, sizeof(header))) {
            fprintf(stderr, "argument %d is not 80 bytes of hex\n", i);
            return 2;
        }
        memset(hash, 0, sizeof(hash));
        if (!minotaur_hash(hash, header, 0)) {
            fprintf(stderr, "minotaur_hash declined argument %d\n", i);
            return 2;
        }
        for (size_t b = 0; b < sizeof(hash); b++) {
            printf("%02x", hash[b]);
        }
        printf("\n");
    }
    return 0;
}

/* Does this build's MinotaurX agree with Avian's?
 *
 * Everything else in this fork rests on it. A miner whose MinotaurX disagrees
 * with the node's computes garbage shares, and no amount of correct Stratum v2
 * helps.
 *
 * The vectors below are real regtest block headers and the hashes the Avian
 * node itself reported for them, so this checks against the node rather than
 * against another copy of the same code. The matching check in the other
 * direction, Avian Core's own implementation against these same headers, lives
 * in the avian-sv2 workspace as avian-pow; the two cannot be linked into one
 * binary because both vendor yespower and export the same symbol names, so
 * one side's call would resolve into the other's code and agreement would
 * prove nothing.
 */

#include <stdio.h>
#include <string.h>

#include "avian/sv2.h"

struct vector {
    const char *name;
    const char *header_hex;
    /* As the node returns it, not as getblockhash prints it: a block hash is
     * printed reversed. */
    const char *hash_hex;
};

static const struct vector VECTORS[] = {
    {"minotaurx regtest block",
     "00000132779b12392b5348809aea7c9c35e5b46928896ae4c686c017a41e16639db0"
     "06660da3727a04ccf5adfde3ae4cac4770f54e0a37cc482f6699f7dee442f8c68ab3"
     "7d31de6affff7f2000000000",
     "cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52effd23"},
    {"minotaurx regtest block, its child",
     "00000132cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52ef"
     "fd235e18e85e588e466a3689d4f3b675f00dac4998ed309bc84cfd55038775bd04e7"
     "7e31de6affff7f2000000000",
     "bee9620fc38ff31b6f009a13a0c66e17b7c55ec8de79554429ce704d769b927a"},
};

static bool unhex(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned value;
        if (sscanf(hex + i * 2, "%2x", &value) != 1) {
            return false;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static void print_hex(const char *label, const uint8_t *bytes, size_t len)
{
    printf("%s", label);
    for (size_t i = 0; i < len; i++) {
        printf("%02x", bytes[i]);
    }
    printf("\n");
}

int avian_minotaurx_selftest(void)
{
    static struct work_restart fallback[1];
    int failures = 0;

    /* cpuminer's yespower polls work_restart[thr_id].restart in the middle of
     * hashing, so it has to point at real memory before the first hash. main
     * allocates it, but the self-test can be asked for before that. */
    if (!work_restart) {
        work_restart = fallback;
    }

    if (!algo_gate.miner_thread_init(0)) {
        fprintf(stderr, "the algorithm gate would not initialise\n");
        return 2;
    }

    printf("MinotaurX, against hashes a real Avian node reported\n\n");
    for (size_t v = 0; v < sizeof(VECTORS) / sizeof(VECTORS[0]); v++) {
        const struct vector *vec = &VECTORS[v];
        uint32_t header[20] __attribute__((aligned(64)));
        uint32_t hash[8] __attribute__((aligned(64)));
        uint8_t expected[32];

        if (!unhex(vec->header_hex, (uint8_t *)header, 80)
            || !unhex(vec->hash_hex, expected, sizeof(expected))) {
            fprintf(stderr, "%s: bad vector\n", vec->name);
            return 2;
        }
        memset(hash, 0, sizeof(hash));
        /* The gate's hash takes the header in natural byte order. scanhash is
         * what byteswaps, because Stratum v1 delivers big-endian words. */
        if (!algo_gate.hash(hash, header, 0)) {
            fprintf(stderr, "%s: the hash function declined this header\n",
                    vec->name);
            return 2;
        }
        bool agree = memcmp(hash, expected, sizeof(expected)) == 0;
        printf("%s: %s\n", vec->name, agree ? "agrees" : "DISAGREES");
        print_hex("  node  ", expected, sizeof(expected));
        print_hex("  miner ", (const uint8_t *)hash, sizeof(expected));
        if (!agree) {
            failures++;
        }
    }

    printf("\n");
    if (failures) {
        printf("FAIL: %d of %zu headers disagree with the node\n", failures,
               sizeof(VECTORS) / sizeof(VECTORS[0]));
        return 1;
    }
    printf("OK: this build's MinotaurX agrees with the node\n");
    return 0;
}

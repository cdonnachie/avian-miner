/* Does this build's proof of work agree with Avian's?
 *
 * Everything else in this fork rests on it. A miner whose hash disagrees with
 * the node's computes garbage shares, and no amount of correct Stratum v2
 * helps.
 *
 * The vectors are real regtest block headers and the hashes the Avian node
 * itself reported for them, so this checks against the node rather than
 * against another copy of the same code. For X16RT there is one vector per
 * 128-second epoch, because the hash chain comes from
 * SHA256d(nTime & 0xffffff80) and vectors inside one epoch would exercise one
 * chain while looking exactly like a thorough check. They were captured with
 * avian/capture-vectors.sh.
 *
 * It drives the miner's real scanhash rather than calling a hash function
 * directly. That is not fussiness: on an AVX512 build algo_gate.hash for
 * X16RT is the eight-way variant, which wants interleaved data and a prehash
 * only scanhash sets up, so anything simpler here would test a
 * reimplementation of the miner instead of the miner. The target is set to all
 * ones so the first nonce wins, and submit_solution hands the hash back
 * through avian_selftest_capture.
 *
 * The matching check against Avian Core's own implementation, on headers no
 * node ever saw, is avian/compare-with-avian-pow.sh. The two cannot be linked
 * into one binary: both vendor yespower and export the same symbol names, so
 * one side's call resolves into the other's code and agreement would prove
 * nothing.
 */

#include <stdio.h>
#include <string.h>

#include "avian/sv2.h"

struct vector {
    const char *header_hex;
    /* As the node returns it, not as getblockhash prints it: a block hash is
     * printed reversed. */
    const char *hash_hex;
};

static const struct vector MINOTAURX[] = {
    {"00000132779b12392b5348809aea7c9c35e5b46928896ae4c686c017a41e16639db0"
     "06660da3727a04ccf5adfde3ae4cac4770f54e0a37cc482f6699f7dee442f8c68ab3"
     "7d31de6affff7f2000000000",
     "cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52effd23"},
    {"00000132cc3da685bad01d260ca785e17b0f0917dd6ac0be1f98c3f73b638b0e52ef"
     "fd235e18e85e588e466a3689d4f3b675f00dac4998ed309bc84cfd55038775bd04e7"
     "7e31de6affff7f2000000000",
     "bee9620fc38ff31b6f009a13a0c66e17b7c55ec8de79554429ce704d769b927a"},
};

/* Six blocks in six different 128-second epochs, so six different hash
 * chains. One epoch's worth would prove far less than it looks. */
static const struct vector X16RT[] = {
    {"000000227f78cf1d89b20dbe77361d6a87e47bad06399047dda5abe884ed273dd034"
     "3665a784fe96fd3d88f75a19f7583789078768670177ec0490ac6e5acd6f85029641"
     "8086de6affff7f2001000000",
     "87a28dc4b555f3305df832ed3962605f2968329e08e97e8a7841cc5cba415053"},
    {"0000003287a28dc4b555f3305df832ed3962605f2968329e08e97e8a7841cc5cba41"
     "50537aefb40130d830654833e0aa644184421a11d3c59c2728e20c0fe1693a7b7a8f"
     "0087de6affff7f2000000000",
     "f708b4f7ef90d66f058ad25e7b992e9b69d233716eac09d6a608f7a6046f782d"},
    {"00000032f708b4f7ef90d66f058ad25e7b992e9b69d233716eac09d6a608f7a6046f"
     "782d70e29a76e8c88e79f21f23dce548d3e26a1acba9bc0e5f71f0a89b850be1f64f"
     "8087de6affff7f2000000000",
     "cb1eb367e7ce8681c681c398f388c1490ef93714ed73421751dfa54fecb8a535"},
    {"00000032cb1eb367e7ce8681c681c398f388c1490ef93714ed73421751dfa54fecb8"
     "a535b43951aa433bf59042d773b0bf99b7be72a74a3c0e340d9372b7fa7ef6edfd7f"
     "0088de6affff7f2001000000",
     "41a125d8a0aba5c37d57b54699a25379af86c752660b56a84097efa463d2a406"},
    {"0000003241a125d8a0aba5c37d57b54699a25379af86c752660b56a84097efa463d2"
     "a40604e9e803b1c55f24b8636d02ea66ddf97e5732d921e1318403ee1cef883809eb"
     "8088de6affff7f2000000000",
     "4792c016028888c222d2d46e3bdac513752315baa541544b526fd47550cb7f22"},
    {"000000324792c016028888c222d2d46e3bdac513752315baa541544b526fd47550cb"
     "7f22b17f6bef8f58071e92bdbd82e6ebbd6b3de0ea86cf4b4cf541ff4e076871906b"
     "0089de6affff7f2004000000",
     "f73cd553a1b33f4b21c6002e3e7cc525a2300f7d82b44f8bcd56414ac25b672f"},
};

static uint8_t captured[32];
static bool captured_any;
static bool capturing;

/* submit_solution calls this first. It returns false in every ordinary run, so
 * nothing about mining changes; only --avian-selftest turns it on. */
bool avian_selftest_capture(const void *hash)
{
    if (!capturing) {
        return false;
    }
    /* The first one only. A wide scanhash computes eight nonces at once and,
     * with the target all ones, submits all eight; lane 0 is the nonce asked
     * for. */
    if (!captured_any) {
        memcpy(captured, hash, sizeof(captured));
        captured_any = true;
    }
    return true;
}

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

int avian_pow_selftest(void)
{
    static struct work_restart fallback[1];
    struct work work __attribute__((aligned(64)));
    struct thr_info thr;
    const struct vector *vectors;
    size_t count;
    const char *name;
    int failures = 0;

    if (opt_algo == ALGO_MINOTAURX) {
        vectors = MINOTAURX;
        count = sizeof(MINOTAURX) / sizeof(MINOTAURX[0]);
        name = "MinotaurX";
    } else if (opt_algo == ALGO_X16RT) {
        vectors = X16RT;
        count = sizeof(X16RT) / sizeof(X16RT[0]);
        name = "X16RT";
    } else {
        fprintf(stderr, "--avian-selftest knows x16rt and minotaurx, "
                        "not this algorithm\n");
        return 2;
    }

    /* cpuminer's yespower and scanhash_x16rt both read
     * work_restart[thr_id].restart while hashing, so it has to point at real
     * memory. main allocates it, but the self-test runs before that. */
    if (!work_restart) {
        work_restart = fallback;
    }
    if (algo_gate.miner_thread_init && !algo_gate.miner_thread_init(0)) {
        fprintf(stderr, "the algorithm gate would not initialise\n");
        return 2;
    }

    memset(&thr, 0, sizeof(thr));
    thr.id = 0;
    capturing = true;

    printf("%s, against hashes a real Avian node reported\n\n", name);
    for (size_t v = 0; v < count; v++) {
        uint8_t header[80];
        uint8_t expected[32];
        uint64_t hashes_done = 0;

        if (!unhex(vectors[v].header_hex, header, sizeof(header))
            || !unhex(vectors[v].hash_hex, expected, sizeof(expected))) {
            fprintf(stderr, "vector %zu is malformed\n", v + 1);
            return 2;
        }

        memset(&work, 0, sizeof(work));
        /* Stratum v1's convention, which is what scanhash expects: every word
         * byte-reversed against the header, because scanhash reverses it back
         * before hashing. The nonce is the exception, because scanhash
         * overwrites the hashed nonce with pdata[19] as it stands. */
        for (int w = 0; w < 20; w++) {
            work.data[w] = be32_at(header + w * 4);
        }
        work.data[19] = le32_at(header + 76);
        memset(work.target, 0xff, sizeof(work.target));

        captured_any = false;
        memset(captured, 0, sizeof(captured));
        work_restart[0].restart = 0;
        /* Room for one pass of the widest scanhash: a wide one computes its
         * last nonce as max_nonce - 8, so max_nonce = first_nonce + 1 makes
         * that underflow and the loop never end. */
        algo_gate.scanhash(&work, work.data[19] + 16, &hashes_done, &thr);

        if (!captured_any) {
            fprintf(stderr, "vector %zu: the miner computed no hash\n", v + 1);
            return 2;
        }
        bool agree = memcmp(captured, expected, sizeof(expected)) == 0;
        printf("block %zu: %s\n", v + 1, agree ? "agrees" : "DISAGREES");
        print_hex("  node  ", expected, sizeof(expected));
        print_hex("  miner ", captured, sizeof(captured));
        if (!agree) {
            failures++;
        }
    }
    capturing = false;

    printf("\n");
    if (failures) {
        printf("FAIL: %d of %zu headers disagree with the node\n", failures,
               count);
        return 1;
    }
    printf("OK: this build's %s agrees with the node on all %zu blocks\n", name,
           count);
    return 0;
}

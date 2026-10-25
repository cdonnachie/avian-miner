/* Stubs for everything cpu-miner.c and util.c define, for pow-probe.
 *
 * work_restart and valid_hash are NOT here: pow-probe.c defines both,
 * because it needs valid_hash to say yes and work_restart to be real.
 *
 * pow-probe links the miner's hash primitives without its mining loop, so
 * these symbols are referenced by objects that are linked but never called.
 * Nothing here is reachable from minotaur_hash, which is the only thing the
 * harness calls.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "algo-gate-api.h"

bool opt_debug = false;
bool opt_quiet = true;
bool have_stratum = false;
int opt_n_threads = 1;
int opt_param_n = 0;
int opt_param_r = 0;
char *opt_param_key = NULL;
double opt_target_factor = 1.0;
bool opt_randomize = false;
bool opt_sapling = false;
int pk_buffer_size = 26;
char *opt_data_file = NULL;
bool opt_verify = false;
char *rpc_user = NULL;

void applog(int prio, const char *fmt, ...)
{
    va_list ap;
    (void)prio;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void work_free(struct work *w) { (void)w; }
void work_copy(struct work *dest, const struct work *src) { (void)dest; (void)src; }
void cbin2hex(char *out, const char *in, size_t len) { (void)out; (void)in; (void)len; }
void bin2hex(char *s, const unsigned char *p, size_t len) { (void)s; (void)p; (void)len; }
char *abin2hex(const unsigned char *p, size_t len) { (void)p; (void)len; return NULL; }
bool hex2bin(unsigned char *p, const char *hexstr, const size_t len)
{
    (void)p; (void)hexstr; (void)len; return false;
}
void format_number_si(double *hashrate, char *si_units) { (void)hashrate; (void)si_units; }
void algo_not_tested(void) {}
void algo_not_implemented(void) {}
int scanhash_8way_64in_32out(struct work *work, uint32_t max_nonce,
                             uint64_t *hashes_done, struct thr_info *mythr)
{
    (void)work; (void)max_nonce; (void)hashes_done; (void)mythr; return 0;
}
void sha256d_gen_merkle_root(char *merkle_root, struct stratum_ctx *sctx)
{
    (void)merkle_root; (void)sctx;
}
bool std_be_work_decode(struct work *work) { (void)work; return false; }
bool std_be_submit_getwork_result(CURL *curl, struct work *work)
{
    (void)curl; (void)work; return false;
}
void std_be_build_stratum_request(char *req, struct work *work) { (void)req; (void)work; }
void set_work_data_big_endian(struct work *work) { (void)work; }

bool fulltest(const uint32_t *hash, const uint32_t *target)
{
    (void)hash; (void)target; return false;
}
void sha256_gen_merkle_root(char *merkle_root, struct stratum_ctx *sctx)
{
    (void)merkle_root; (void)sctx;
}
void four_way_not_tested(void) {}

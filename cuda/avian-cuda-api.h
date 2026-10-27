#ifndef AVIAN_CUDA_API_H
#define AVIAN_CUDA_API_H 1

/* The boundary between the miner and its CUDA kernels.
 *
 * On Windows the kernels have to live in a DLL, because nvcc will only use
 * MSVC as its host compiler while the miner and its Rust Stratum v2 library
 * are MinGW. That makes this a real ABI rather than an internal interface,
 * so it carries only plain data: no struct work, whose layout would have to
 * match across two compilers, and no thread-local state, because a DLL's TLS
 * is a separate block from the executable's.
 *
 * The division of labour follows from that. The kernels find candidate
 * nonces; the miner verifies them against its own X16RT and submits. Keeping
 * verification on the miner's side is not only convenient: it is the scalar
 * path that --avian-selftest now covers, and it is what stands between a
 * wrong GPU hash and a bad share.
 *
 * Linux links the same function directly, so there is one implementation
 * rather than a Windows-only path that nothing exercises.
 */

#include <stdint.h>

#ifdef _WIN32
#  ifdef AVIAN_CUDA_BUILDING_DLL
#    define AVIAN_CUDA_API __declspec(dllexport)
#  else
#    define AVIAN_CUDA_API __declspec(dllimport)
#  endif
#else
#  define AVIAN_CUDA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Everything one batch needs. Plain data, so the layout is the same whichever
 * compiler built either side. */
typedef struct {
    uint32_t data[20];    /* work->data, the header in byte-swapped words */
    uint32_t target[8];
    char     order[20];   /* the chain, as 16 hex characters plus a NUL */
    uint32_t start_nonce;
    uint32_t max_nonce;
    int      thr_id;
    int      benchmark;
    int      quiet;
    int      debug;
    /* Polled between kernels so a new job can cut a batch short. NULL means
     * never abort. Read through the pointer rather than copied, because the
     * miner sets it while the batch is running. */
    const volatile unsigned char *restart;
} avian_cuda_job;

/* One batch. Returns the number of candidate nonces written to out_nonces,
 * 0 if the batch found nothing, or negative on error. Candidates are absolute
 * nonces, already adjusted for the batch's start.
 *
 * A candidate is a nonce the device believes is under target. The caller must
 * still hash it and check, because that is the only thing standing between a
 * kernel defect and a bad share. */
AVIAN_CUDA_API int avian_cuda_scan_batch(const avian_cuda_job *job,
                                         uint32_t *out_nonces, int max_out,
                                         uint32_t *throughput_used);

AVIAN_CUDA_API int avian_cuda_device_count(void);
AVIAN_CUDA_API const char *avian_cuda_device_name(int dev_id);

/* Logging. The kernels have plenty to say but no business owning the
 * miner's log, so the miner hands one in. Unset means silence. */
typedef void (*avian_cuda_log_fn)(int level, const char *msg);
AVIAN_CUDA_API void avian_cuda_set_logger(avian_cuda_log_fn fn);

/* Diagnostics only, and optional. The stage bisect compares the device's
 * intermediates against the miner's CPU chain, which means calling back into
 * the miner. Left unset, the bisect disables itself rather than guessing.
 * Consensus verification never comes through here: that stays in the miner. */
typedef int  (*avian_cuda_hash_n_fn)(void *out, const void *in, int thr_id,
                                     const char *order, int stages);
typedef void (*avian_cuda_prehash_fn)(void *edata, void *pdata, const char *order);
AVIAN_CUDA_API void avian_cuda_set_diagnostics(avian_cuda_hash_n_fn hash_n,
                                               avian_cuda_prehash_fn prehash);

#ifdef __cplusplus
}
#endif

#endif /* AVIAN_CUDA_API_H */

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
 * Linux builds the same thing as a shared object rather than linking the
 * objects in, so there is one implementation and one code path rather than a
 * Windows-only one that nothing exercises. Both vendors' libraries export
 * the names below, and the miner loads them; see cuda/avian-gpu-backend.h.
 */

#include <stdint.h>

#if defined(_WIN32) && defined(AVIAN_CUDA_BUILDING_DLL)
#  define AVIAN_CUDA_API __declspec(dllexport)
#else
/* Deliberately not dllimport. Both vendors' libraries export these same
 * names and neither is statically imported: the miner loads them and reads
 * the entry points into a table, which is what lets one binary drive either
 * or both. See cuda/avian-gpu-backend.h. A dllimport here would put the
 * names back in the import table and bind them to one library again. */
#  define AVIAN_CUDA_API
#endif

/* Bumped whenever avian_cuda_job or an entry point changes shape. The miner
 * refuses a backend that disagrees, because the struct is plain data with
 * nothing in it to detect a mismatch from: a stale library would simply read
 * the fields in the wrong places.
 *
 *   1  the original: thr_id selected the device through the backend's own
 *      device_map, which only works when there is one backend
 *   2  job.device names the device, so two backends can each number from
 *      zero, and avian_cuda_abi_version exists to say so */
#define AVIAN_CUDA_ABI_VERSION 4

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
    /* The backend's own device index, which is not thr_id. With two backends
     * loaded each numbers its devices from zero, so the miner's flat
     * numbering has to be translated before it crosses this boundary, and
     * avian-gpu-backend.c is the only thing that knows how. */
    int      device;
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

/* AVIAN_CUDA_ABI_VERSION as this library was built with it. */
AVIAN_CUDA_API int avian_cuda_abi_version(void);

AVIAN_CUDA_API int avian_cuda_device_count(void);
AVIAN_CUDA_API const char *avian_cuda_device_name(int dev_id);

/* What the card is, not what found it: "nvidia", "amd", "intel", or "" when
 * the backend cannot tell. Lower case and stable, because it is matched
 * rather than printed.
 *
 * Added in ABI 3, and the reason is that only this library knows. The CUDA
 * and HIP backends answer with a constant, since each drives one vendor. The
 * OpenCL one drives any of them, so without this the loader cannot tell that
 * its "gfx942" and the HIP backend's "AMD Instinct MI325X" are one card, and
 * a machine with both offers the same silicon twice under two numbers. That
 * is what --gpu-backend needs to resolve, and name strings cannot do it: the
 * two backends do not describe a card the same way.
 */
AVIAN_CUDA_API const char *avian_cuda_device_vendor(int dev_id);

/* The card's PCI bus number, or -1 when this backend cannot tell.
 *
 * Added in ABI 4, for the same reason as the vendor above: only the library
 * knows. A mining distribution identifies a card by its PCIe slot, because
 * that is the one name that does not move when a driver renumbers or a
 * second vendor's runtime loads, and it is what HiveOS pairs a hash rate
 * with to fill the right row of its GPU table. Without it a rig with cards
 * of both vendors shows its rates against the wrong cards.
 *
 * The bus alone, not the whole domain:bus:device.function. It is what the
 * consumers want, it fits an int, and on any machine this runs on no two
 * GPUs share a bus.
 *
 * -1 rather than 0 for unknown, because bus 0 exists and an integrated GPU
 * can be on it.
 */
AVIAN_CUDA_API int avian_cuda_device_pci_bus(int dev_id);

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

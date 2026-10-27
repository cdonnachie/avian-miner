#ifndef AVIAN_X16RT_CUDA_H
#define AVIAN_X16RT_CUDA_H 1

/* The CUDA X16RT driver, and the CPU-side functions it borrows.
 *
 * Those functions are declared here rather than by including x16r-gate.h in
 * the .cu file, because that header reaches simd-utils.h and a translation
 * unit full of host SIMD intrinsics is not something to hand nvcc. The
 * declarations are therefore duplicated, which is a drift risk, so
 * x16rt-cuda-gate.c includes this header and x16r-gate.h together: a
 * disagreement between them is then a compile error there rather than
 * undefined behaviour at run time.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* register_x16rt_cuda_algo is declared in x16r-gate.h, beside the other
 * register_*_algo functions, because algo_gate_t is an anonymous struct
 * typedef and cannot be forward declared here. */

struct work;
struct thr_info;

/* The scanhash this build would have used without --gpu. */
typedef int (*x16rt_scanhash_fn)( struct work *, uint32_t, uint64_t *,
                                  struct thr_info * );

/* Where the GPU driver sends work it cannot do correctly. Measured: chains
 * beginning with SIMD yield 0 usable candidates out of 397, so they go here
 * until the 80-byte SIMD kernel is fixed. */
void x16rt_cuda_set_cpu_fallback( x16rt_scanhash_fn fn );

/* Same shape as the algo gate's scanhash, so it can be registered directly. */
int scanhash_x16rt_cuda( struct work *work, uint32_t max_nonce,
                         uint64_t *hashes_done, struct thr_info *mythr );

/* Borrowed from the CPU implementation. Reusing these rather than carrying a
 * second copy matters most for the first two: the hash order derivation is
 * consensus critical, and one definition cannot disagree with itself. */
void x16rt_getTimeHash( const uint32_t timeStamp, void *timeHash );
void x16rt_getAlgoString( const uint32_t *timeHash, char *output );
void x16rt_log_hash_order( const char *order, const uint32_t ntime );
void x16r_prehash( void *edata, void *pdata, const char *order );
int  x16r_hash( void *output, const void *input, int thrid );
/* The chain truncated to func_count stages, writing the 64-byte
 * intermediate. The stage bisect compares it against the device's
 * d_hash after the same number of kernels. */
int  x16r_hash_generic( void *output, const void *input, int thrid,
                        const char *hash_order, const int func_count );

#ifdef __cplusplus
}
#endif

#endif /* AVIAN_X16RT_CUDA_H */

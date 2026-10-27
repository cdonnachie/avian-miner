/* Registering the CUDA X16RT backend in the algo gate.
 *
 * Separate from x16r-gate.c so that nvcc and the C compiler each see only
 * what they can digest, and separate from x16rt-cuda.cu so that this file can
 * include x16r-gate.h. That matters: x16rt-cuda.h redeclares the CPU
 * functions the driver borrows, because pulling x16r-gate.h into a .cu would
 * drag simd-utils.h and a pile of host SIMD intrinsics in with it. Including
 * both headers here turns any disagreement between those declarations into a
 * compile error rather than something that goes wrong at run time.
 */

#include "algo-gate-api.h"
#include "algo/x16/x16r-gate.h"
#include "algo/x16/x16rt-cuda.h"
#include "cuda/avian-cuda-device.h"

bool register_x16rt_cuda_algo( algo_gate_t* gate )
{
   if ( cuda_detect_devices() == 0 )
   {
      applog( LOG_ERR, "no CUDA device found, so --gpu cannot be honoured" );
      return false;
   }
   cuda_print_devices();

   /* Kept before it is overwritten. The 80-byte SIMD kernel reports only
    * bad candidates, so a chain starting with SIMD is mined on the CPU
    * instead; see x16rt_cuda_set_cpu_fallback. */
   x16rt_cuda_set_cpu_fallback( (x16rt_scanhash_fn) gate->scanhash );
   gate->scanhash = (void*)&scanhash_x16rt_cuda;

   /* The CPU hash stays registered. It is not dead weight: the driver checks
    * every candidate the GPU reports against it before submitting, so the
    * optimisations below are still required rather than vestigial. */
   gate->hash = (void*)&x16r_hash;
   gate->optimizations = SSE2_OPT | AES_OPT | AVX2_OPT | AVX512_OPT | VAES_OPT
                       | NEON_OPT;
   opt_target_factor = 256.0;

   /* One mining thread per device. The gate's default get_new_work splits the
    * nonce range by opt_n_threads, which is then exactly one range per card.
    * Set here rather than left to the CPU count, where sixteen threads would
    * each claim a sixteenth of the range and all drive the same GPU. */
   if ( opt_n_threads != cuda_num_devices )
   {
      applog( LOG_INFO, "using %d mining thread%s, one per GPU",
              cuda_num_devices, cuda_num_devices == 1 ? "" : "s" );
      opt_n_threads = cuda_num_devices;
   }

   return true;
}

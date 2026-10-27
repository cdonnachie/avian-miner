/* Registering the CUDA X16RT backend in the algo gate.
 *
 * Separate from x16r-gate.c so that nvcc and the C compiler each see only
 * what they can digest, and separate from the driver so that this file can
 * include x16r-gate.h. That matters: x16rt-cuda.h redeclares the CPU
 * functions the backend borrows, because pulling x16r-gate.h into a .cu would
 * drag simd-utils.h and a pile of host SIMD intrinsics in with it. Including
 * both headers here turns any disagreement between those declarations into a
 * compile error rather than something that goes wrong at run time.
 *
 * Talks to the kernels only through cuda/avian-cuda-api.h, never to
 * device.cu's globals. On Windows device.cu lives inside the DLL, so its
 * cuda_detect_devices and cuda_num_devices are not linkable from here at
 * all. Going through the API means one gate serves both platforms.
 */

#include "algo-gate-api.h"
#include "algo/x16/x16r-gate.h"
#include "algo/x16/x16rt-cuda.h"
#include "cuda/avian-cuda-api.h"

/* The kernels have plenty to say and no business owning the miner's log, so
 * they are handed one. On Linux they could reach applog directly, but going
 * through the callback on both platforms keeps startup identical. */
static void x16rt_cuda_log( int level, const char *msg )
{
   applog( level, "%s", msg );
}

bool register_x16rt_cuda_algo( algo_gate_t* gate )
{
   int devices;

   avian_cuda_set_logger( x16rt_cuda_log );

   devices = avian_cuda_device_count();
   if ( devices <= 0 )
   {
      applog( LOG_ERR, "no CUDA device found, so --gpu cannot be honoured" );
      return false;
   }
   for ( int d = 0; d < devices; d++ )
      applog( LOG_INFO, "GPU #%d: %s", d, avian_cuda_device_name( d ) );

   gate->scanhash = (void*)&scanhash_x16rt_cuda;

   /* The CPU hash stays registered, and is not dead weight: the backend
    * checks every candidate the device reports against it before submitting.
    * That check is the only thing standing between a kernel defect and a bad
    * share, so the optimisations below are required rather than vestigial.
    *
    * There used to be a CPU fallback here for chains beginning with SIMD,
    * on the belief that the 80-byte SIMD kernel reported only bad
    * candidates. It did not: the fault was in x16r.c, which hashed an
    * uninitialised buffer when SIMD led the chain, so the kernel was right
    * and the verifier was wrong. Fixed in 28a0015, and the fallback is gone
    * with it. */
   gate->hash = (void*)&x16r_hash;
   gate->optimizations = SSE2_OPT | AES_OPT | AVX2_OPT | AVX512_OPT | VAES_OPT
                       | NEON_OPT;
   opt_target_factor = 256.0;

   /* One mining thread per device. The gate's default get_new_work splits the
    * nonce range by opt_n_threads, which is then exactly one range per card.
    * Set here rather than left to the CPU count, where sixteen threads would
    * each claim a sixteenth of the range and all drive the same GPU. */
   if ( opt_n_threads != devices )
   {
      applog( LOG_INFO, "using %d mining thread%s, one per GPU",
              devices, devices == 1 ? "" : "s" );
      opt_n_threads = devices;
   }

   return true;
}

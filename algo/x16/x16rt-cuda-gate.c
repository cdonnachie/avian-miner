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
#include "cuda/avian-gpu-backend.h"

/* The kernels have plenty to say and no business owning the miner's log, so
 * they are handed one. Not a stylistic choice any more: the backends are
 * loaded libraries on both platforms now and cannot reach the executable's
 * applog at all, so this callback is the only way a line gets back here. */
static void x16rt_cuda_log( int level, const char *msg )
{
   applog( level, "%s", msg );
}

bool register_x16rt_cuda_algo( algo_gate_t* gate )
{
   int devices;

   /* Loads whichever vendors are present and numbers their devices as one
    * list, so the rest of the miner, and every "GPU #n" below, means one
    * thing whether this machine has Nvidia cards, AMD cards or both. */
   devices = avian_gpu_init( x16rt_cuda_log );
   if ( devices <= 0 )
   {
      /* Two different faults with one symptom, which is worth separating
       * here rather than in a support thread. Note the second keeps the
       * exact words package-amd-test.ps1 looks for to decide whether its
       * retry is worth running: that retry swaps the shipped runtime for the
       * driver's, which only makes sense once a library has loaded. */
      if ( avian_gpu_backend_present() == 0 )
         applog( LOG_ERR, "no GPU backend beside the miner, so --gpu cannot be "
                          "honoured: expected %s or %s in this directory",
                 AVIAN_GPU_NVIDIA_LIB_NAME, AVIAN_GPU_AMD_LIB_NAME );
      else if ( avian_gpu_backend_count() == 0 )
         /* The libraries are right there, so telling anyone to go and look
          * for them sends them to the wrong place: what is missing is a
          * driver. Counting loads rather than files said exactly that. */
         applog( LOG_ERR, "a GPU backend is beside the miner but none would "
                          "load, so --gpu cannot be honoured: this machine "
                          "has no NVIDIA or AMD runtime installed" );
      else
         applog( LOG_ERR, "no CUDA device found, so --gpu cannot be honoured" );
      return false;
   }
   for ( int d = 0; d < devices; d++ )
      applog( LOG_INFO, "GPU #%d: %s (%s)", d, avian_gpu_device_name( d ),
              avian_gpu_vendor( d ) );

   gate->scanhash = (void*)&scanhash_x16rt_cuda;
   gpu_backend_active = true;

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

   /* One mining thread per device, across both vendors: thread n drives
    * device n, which is what x16rt-cuda-host.c passes to the loader. The
    * gate's default get_new_work splits the nonce range by opt_n_threads,
    * which is then exactly one range per card.
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

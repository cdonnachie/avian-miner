#ifndef AVIAN_GPU_BACKEND_H
#define AVIAN_GPU_BACKEND_H 1

/* One or both vendors, behind one device numbering.
 *
 * The kernels are the same source compiled twice, by nvcc for Nvidia and by
 * hipcc for AMD, into two libraries exporting the identical avian_cuda_*
 * names. Two libraries cannot both satisfy a static import, so nothing here
 * imports them: each is loaded explicitly and its entry points are read into
 * a table. The shared export names then stop being a collision and become
 * the point.
 *
 * Loading rather than importing also decides what happens when a vendor is
 * absent, which is the normal case. A static import of a missing library is
 * a process that will not start; a load that fails is a backend that is
 * skipped, so one binary runs on an Nvidia machine, an AMD machine, or one
 * with both, without being rebuilt.
 *
 * Devices are numbered across both backends, Nvidia first, so --gpu and the
 * "GPU #n" in every log line mean one thing rather than two that collide at
 * zero.
 */

#include <stdint.h>

#include "cuda/avian-cuda-api.h"

/* What is looked for, beside the executable first and then on the loader's
 * own search path. Two names rather than one because two files cannot share
 * a name in a directory, which is the only reason the AMD library is not
 * also called avian_cuda. */
#ifdef _WIN32
#  define AVIAN_GPU_NVIDIA_LIB_NAME "avian_cuda.dll"
#  define AVIAN_GPU_AMD_LIB_NAME    "avian_amd.dll"
#else
#  define AVIAN_GPU_NVIDIA_LIB_NAME "libavian_cuda.so"
#  define AVIAN_GPU_AMD_LIB_NAME    "libavian_amd.so"
#endif

/* Loads every backend present and takes its device inventory. Returns the
 * number of devices found across all of them, or 0 if there are none, which
 * includes the case where no backend library could be loaded at all.
 *
 * The logger is handed to each backend as it loads, so a failure inside one
 * is reported through the miner's log rather than lost. */
int avian_gpu_init( avian_cuda_log_fn log );

/* Devices across all backends. Zero before avian_gpu_init. */
int avian_gpu_device_count( void );

/* Backend libraries that loaded, 0 to 2. Worth asking separately from the
 * device count: no library is a packaging problem, while a library that
 * loaded and found nothing is a driver or a hardware one, and the two have
 * nothing in common but the symptom. */
int avian_gpu_backend_count( void );

/* The card's own name, or "" for an index out of range. */
const char *avian_gpu_device_name( int device );

/* Which backend owns this device, for logging: "Nvidia" or "AMD". */
const char *avian_gpu_vendor( int device );

/* Handed to every backend, since a batch may run on any of them. */
void avian_gpu_set_diagnostics( avian_cuda_hash_n_fn hash_n,
                                avian_cuda_prehash_fn prehash );

/* One batch on one device, routed to the backend that owns it.
 *
 * `job` is written rather than only read: the device field is set to the
 * backend-local index, because each backend numbers from zero and only this
 * layer knows the mapping. Returns what avian_cuda_scan_batch returns, or
 * -1 for a device index that belongs to nothing. */
int avian_gpu_scan_batch( int device, avian_cuda_job *job,
                          uint32_t *out_nonces, int max_out,
                          uint32_t *throughput_used );

#endif /* AVIAN_GPU_BACKEND_H */

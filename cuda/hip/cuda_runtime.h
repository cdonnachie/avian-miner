/* Give the CUDA kernels the names they expect on top of HIP.
 *
 * Everything here is a rename. Where a CUDA name has a HIP equivalent with
 * the same meaning, it is mapped; where a construct needed the kernels
 * themselves to change, it is not here but in one of the patch scripts under
 * scripts/. That split is deliberate and is how the port stayed reviewable:
 * this file absorbs no semantics.
 *
 * It is reached by putting cuda/hip on the include path ahead of anything
 * else, so a kernel's `#include <cuda_runtime.h>` finds this rather than the
 * CUDA toolkit's. An Nvidia build never sees it.
 */
#ifndef AVIAN_CUDASHIM_H
#define AVIAN_CUDASHIM_H 1

#include <hip/hip_runtime.h>

/* CUDA annotates its own declarations with these. They carry no meaning for
 * a compiler that is not nvcc. */
#define __device_builtin__
#define __host_builtin__
#define __device_builtin_texture_type__
#define __device_builtin_surface_type__
#define __cudart_builtin__

/* nvcc provides these; HIP does not. __builtin_align__ is the big one: it is
 * used to declare uint48 in cuda_vectors_alexis.h, and without it that one
 * typedef fails and every later use of uint48 fails with it. One missing
 * macro accounted for 228 of 357 error reports.
 */
#define __builtin_align__(n) __attribute__((aligned(n)))
#ifndef __align__
#  define __align__(n) __attribute__((aligned(n)))
#endif

#define cudaMallocHost hipHostMalloc

/* The runtime API, as far as the kernels' host side reaches into it. */
typedef hipError_t  cudaError_t;
typedef hipStream_t cudaStream_t;
typedef hipEvent_t  cudaEvent_t;
typedef hipDeviceProp_t cudaDeviceProp;

#define cudaSuccess            hipSuccess
#define cudaGetErrorString     hipGetErrorString
#define cudaGetLastError       hipGetLastError
#define cudaDeviceSynchronize  hipDeviceSynchronize
#define cudaStreamSynchronize  hipStreamSynchronize
#define cudaMalloc             hipMalloc
#define cudaFree               hipFree
#define cudaMemcpy             hipMemcpy
#define cudaMemcpyAsync        hipMemcpyAsync
#define cudaMemset             hipMemset
#define cudaMemcpyToSymbol     hipMemcpyToSymbol
#define cudaMemcpyHostToDevice hipMemcpyHostToDevice
#define cudaMemcpyDeviceToHost hipMemcpyDeviceToHost
#define cudaGetDeviceProperties hipGetDeviceProperties
#define cudaSetDevice          hipSetDevice
#define cudaGetDeviceCount     hipGetDeviceCount
#define cudaFuncCachePreferL1  hipFuncCachePreferL1
#define cudaDeviceSetCacheConfig hipDeviceSetCacheConfig
#define cudaHostAlloc          hipHostMalloc
#define cudaFreeHost           hipHostFree
#define cudaHostAllocMapped    hipHostMallocMapped
#define cudaSetDeviceFlags     hipSetDeviceFlags

/* Deprecated in CUDA 4 and still present in 12.9, which is why the kernels
 * can go on calling it and why this is a rename rather than an edit to
 * cuda_checkhash.cu. HIP dropped the thread-scoped spelling entirely; the
 * device-scoped one it kept has the same meaning, since the distinction went
 * away when CUDA stopped having per-thread contexts. */
#define cudaThreadSynchronize  hipDeviceSynchronize

/* A flag for cudaSetDeviceFlags rather than a function, so there is no CUDA
 * spelling that HIP also accepts and no way to avoid naming it here. */
#define cudaDeviceScheduleBlockingSync hipDeviceScheduleBlockingSync

#endif /* AVIAN_CUDASHIM_H */

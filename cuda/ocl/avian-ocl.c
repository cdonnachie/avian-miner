/* The OpenCL backend: a third sibling to libavian_cuda.so and
 * libavian_amd.so, exporting the same avian_cuda_* ABI and loaded the same
 * way. See cuda/avian-cuda-api.h for the boundary and
 * cuda/avian-gpu-backend.h for the loading.
 *
 * Why a third one at all, when the HIP backend already covers AMD: a mining
 * distribution ships an OpenCL stack and maintains it, and does not ship
 * HIP. So this needs nothing installed where the HIP backend needs a ROCm
 * runtime that is not there. It also reaches Intel, which neither of the
 * other two does.
 *
 * Two structural differences from its siblings, both in its favour. There is
 * no device compiler at build time, because OpenCL compiles kernels at run
 * time, so this is plain C and there is no architecture list to keep: a card
 * that does not exist yet still works. And the kernels are text, so they
 * ship as source rather than as a forty-megabyte fatbinary.
 *
 * Targets OpenCL 1.2 deliberately. HiveOS carries ROCm 5.6.1's OpenCL and
 * its devices report OpenCL C 1.2, so anything newer would build here and
 * fail on the machines this exists for.
 */

#define CL_TARGET_OPENCL_VERSION 120

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CL/cl.h>

#include "cuda/avian-cuda-api.h"

/* Enough for any rig. The loader has its own limit and will say so first. */
#define AVIAN_OCL_MAX_DEVICES 32

static avian_cuda_log_fn g_log = NULL;

static struct
{
    cl_device_id   id;
    cl_platform_id platform;
    char           name[256];
} s_device[AVIAN_OCL_MAX_DEVICES];

static int  s_devices     = 0;
static int  s_enumerated  = 0;

/* Formatted here so the miner receives a finished line, the same bargain
 * avian-cuda-support.cu makes: the kernels have plenty to say and no way to
 * say it until the miner hands a logger in. Silent until then, rather than
 * writing to a stdout nobody is reading. */
static void ocl_log( int level, const char *fmt, ... )
{
    char buf[1024];
    va_list ap;

    if ( g_log == NULL )
        return;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof buf, fmt, ap );
    va_end( ap );
    buf[sizeof buf - 1] = '\0';
    g_log( level, buf );
}

/* LOG_ERR and LOG_INFO as the miner numbers them. Not included from the
 * miner's headers, because this library is built standalone and that is the
 * whole point of the plain-data boundary. */
#define OCL_LOG_ERR     3
#define OCL_LOG_WARNING 4
#define OCL_LOG_INFO    6

static void enumerate( void )
{
    cl_platform_id platform[8];
    cl_uint        platforms = 0;
    cl_device_type want = CL_DEVICE_TYPE_GPU;

    if ( s_enumerated )
        return;
    s_enumerated = 1;

    /* GPUs only. An OpenCL CPU device would be strictly worse than this
     * miner's own CPU path, which is hand-vectorised, so offering one would
     * only invite someone to mine slower by accident. The override exists
     * because POCL's CPU device is the only way to exercise this backend on
     * a machine with no OpenCL GPU, which is how it was developed. */
    if ( getenv( "AVIAN_OCL_ALLOW_CPU" ) != NULL )
        want = CL_DEVICE_TYPE_ALL;

    if ( clGetPlatformIDs( 8, platform, &platforms ) != CL_SUCCESS || platforms == 0 )
    {
        ocl_log( OCL_LOG_INFO, "OpenCL: no platform; this backend has nothing to do" );
        return;
    }

    for ( cl_uint p = 0; p < platforms && s_devices < AVIAN_OCL_MAX_DEVICES; p++ )
    {
        cl_device_id ids[AVIAN_OCL_MAX_DEVICES];
        cl_uint      n = 0;
        char         pname[128] = { 0 };

        clGetPlatformInfo( platform[p], CL_PLATFORM_NAME, sizeof pname, pname, NULL );

        /* A platform with no matching device is ordinary, not an error: an
         * AMD ICD is registered on every machine that ever installed ROCm,
         * including the ones with no AMD card in them. */
        if ( clGetDeviceIDs( platform[p], want, AVIAN_OCL_MAX_DEVICES, ids, &n )
             != CL_SUCCESS || n == 0 )
            continue;

        for ( cl_uint d = 0; d < n && s_devices < AVIAN_OCL_MAX_DEVICES; d++ )
        {
            s_device[s_devices].id       = ids[d];
            s_device[s_devices].platform = platform[p];
            s_device[s_devices].name[0]  = '\0';
            clGetDeviceInfo( ids[d], CL_DEVICE_NAME,
                             sizeof s_device[s_devices].name,
                             s_device[s_devices].name, NULL );
            ocl_log( OCL_LOG_INFO, "OpenCL: %s on %s",
                     s_device[s_devices].name, pname );
            s_devices++;
        }
    }
}

AVIAN_CUDA_API int avian_cuda_abi_version( void )
{
    return AVIAN_CUDA_ABI_VERSION;
}

AVIAN_CUDA_API int avian_cuda_device_count( void )
{
    enumerate();

    /* Deliberately zero while the kernels do not exist yet, so the miner
     * routes no work here and nobody meets a backend that enumerates a card
     * and then fails every batch on it. The devices above are still logged,
     * which is what makes this stage testable. Returns s_devices once
     * avian_cuda_scan_batch does something. */
    if ( s_devices > 0 )
        ocl_log( OCL_LOG_WARNING,
                 "OpenCL: %d device(s) found, but this backend cannot hash yet; "
                 "ignoring them", s_devices );
    return 0;
}

AVIAN_CUDA_API const char *avian_cuda_device_name( int dev_id )
{
    enumerate();
    if ( dev_id < 0 || dev_id >= s_devices )
        return "";
    return s_device[dev_id].name;
}

AVIAN_CUDA_API void avian_cuda_set_logger( avian_cuda_log_fn fn )
{
    g_log = fn;
}

AVIAN_CUDA_API void avian_cuda_set_diagnostics( avian_cuda_hash_n_fn hash_n,
                                                avian_cuda_prehash_fn prehash )
{
    /* The stage bisect compares device intermediates against the miner's CPU
     * chain, which is how each kernel will be verified as it lands. Taken
     * now so the entry point exists and the ABI is complete; unused until
     * there are stages to compare. */
    (void) hash_n;
    (void) prehash;
}

AVIAN_CUDA_API int avian_cuda_scan_batch( const avian_cuda_job *job,
                                          uint32_t *out_nonces, int max_out,
                                          uint32_t *throughput_used )
{
    (void) job;
    (void) out_nonces;
    (void) max_out;
    if ( throughput_used != NULL )
        *throughput_used = 0;

    /* Unreachable while avian_cuda_device_count returns 0, since the miner
     * routes work by device. Here so the entry point exists, because the
     * loader refuses a library missing any one of them. */
    ocl_log( OCL_LOG_ERR, "OpenCL: scan_batch is not implemented" );
    return -1;
}

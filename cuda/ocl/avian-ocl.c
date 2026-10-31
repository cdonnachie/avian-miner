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
#include "cuda/ocl/avian-ocl-kernels.h"

/* Enough for any rig. The loader has its own limit and will say so first. */
#define AVIAN_OCL_MAX_DEVICES 32

/* One batch's nonces. Deliberately modest: this backend has never run on an
 * AMD card, and a first version that is slow and finishes is worth more than
 * a fast one that trips a display driver's watchdog. Phase 5 measures it.
 *
 * A power of two so the rounding below leaves it alone. */
#define AVIAN_OCL_THROUGHPUT ( 1u << 20 )

/* Candidates per batch. The miner re-hashes each one, so this only has to
 * exceed the genuine solutions plus whatever false positives a 64-bit compare
 * admits, which at any real difficulty is zero or one. */
#define AVIAN_OCL_MAX_CANDIDATES 16

/* The sixteen algorithms in the order x16r_Algo numbers them, which is what a
 * job's order string indexes. Each name is both of its kernels' prefix. */
static const char *const s_algo[16] = {
    "blake512", "bmw512", "groestl512", "jh512",
    "keccak512", "skein512", "luffa512", "cubehash512",
    "shavite512", "simd512", "echo512", "hamsi512",
    "fugue512", "shabal512", "whirlpool512", "sha512"
};

static avian_cuda_log_fn g_log = NULL;

struct ocl_device
{
    cl_device_id     id;
    cl_platform_id   platform;
    char             name[256];

    /* Built on first use rather than at enumeration, so listing devices stays
     * cheap and a machine that never mines never pays for a compile. */
    int              ready;
    int              broken;        /* tried and failed; do not retry */

    cl_context       cx;
    cl_command_queue q;
    cl_program       prog;
    cl_kernel        k64[16];       /* the 64-byte rounds */
    cl_kernel        k80[16];       /* the 80-byte first rounds */
    cl_kernel        kcheck;

    cl_mem           m_header;      /* 80 bytes, one per batch */
    cl_mem           m_hash;        /* throughput * 64 bytes */
    cl_mem           m_out;         /* the count, then the candidates */

    uint32_t         throughput;

    /* The work group size every kernel in the chain can take, a power of two.
     * Settled per device in prepare_device, not guessed here. */
    size_t           local;
};

static struct ocl_device s_device[AVIAN_OCL_MAX_DEVICES];
static int  s_devices    = 0;
static int  s_enumerated = 0;

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
    ocl_log( OCL_LOG_INFO, "OpenCL: %u platform%s", platforms,
             platforms == 1 ? "" : "s" );

    for ( cl_uint p = 0; p < platforms && s_devices < AVIAN_OCL_MAX_DEVICES; p++ )
    {
        cl_device_id ids[AVIAN_OCL_MAX_DEVICES];
        cl_uint      n = 0;
        char         pname[128] = { 0 };

        clGetPlatformInfo( platform[p], CL_PLATFORM_NAME, sizeof pname, pname, NULL );

        /* A platform with no matching device is ordinary, not an error: an
         * AMD ICD is registered on every machine that ever installed ROCm,
         * including the ones with no AMD card in them. Still worth a line,
         * because "no OpenCL device" with no explanation is the least useful
         * thing this backend can say to someone whose card is right there. */
        if ( clGetDeviceIDs( platform[p], want, AVIAN_OCL_MAX_DEVICES, ids, &n )
             != CL_SUCCESS || n == 0 )
        {
            ocl_log( OCL_LOG_INFO, "OpenCL: %s has no GPU device", pname );
            continue;
        }

        for ( cl_uint d = 0; d < n && s_devices < AVIAN_OCL_MAX_DEVICES; d++ )
        {
            struct ocl_device *dev = &s_device[s_devices];

            dev->id         = ids[d];
            dev->platform   = platform[p];
            dev->throughput = AVIAN_OCL_THROUGHPUT;
            dev->name[0]    = '\0';
            clGetDeviceInfo( ids[d], CL_DEVICE_NAME, sizeof dev->name,
                             dev->name, NULL );
            ocl_log( OCL_LOG_INFO, "OpenCL: %s on %s", dev->name, pname );
            s_devices++;
        }
    }
}

static void log_build_failure( struct ocl_device *dev )
{
    size_t n = 0;
    char  *log;

    clGetProgramBuildInfo( dev->prog, dev->id, CL_PROGRAM_BUILD_LOG, 0, NULL, &n );
    if ( n <= 1 )
        return;
    log = malloc( n + 1 );
    if ( log == NULL )
        return;
    clGetProgramBuildInfo( dev->prog, dev->id, CL_PROGRAM_BUILD_LOG, n, log, NULL );
    log[n] = '\0';
    ocl_log( OCL_LOG_ERR, "OpenCL: %s", log );
    free( log );
}

/* Every kernel in one program.
 *
 * The sources are handed to clCreateProgramWithSource as that many strings,
 * which it concatenates. That works because each is self-contained with its
 * dependencies inlined and every dependency carries an include guard, so the
 * second and later copies of each compile to nothing. One build and, once
 * phase 4 caches it, one binary; building them separately would be one of
 * each per kernel. */
static int build_program( struct ocl_device *dev )
{
    const char *src[AVIAN_OCL_KERNEL_COUNT];
    size_t      len[AVIAN_OCL_KERNEL_COUNT];
    cl_int      err;

    for ( size_t i = 0; i < AVIAN_OCL_KERNEL_COUNT; i++ )
    {
        src[i] = avian_ocl_kernels[i].source;
        len[i] = strlen( src[i] );
    }

    dev->prog = clCreateProgramWithSource( dev->cx, AVIAN_OCL_KERNEL_COUNT,
                                           src, len, &err );
    if ( err != CL_SUCCESS )
    {
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: clCreateProgramWithSource failed (%d)",
                 dev->name, err );
        return 0;
    }
    if ( clBuildProgram( dev->prog, 1, &dev->id, "", NULL, NULL ) != CL_SUCCESS )
    {
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: the kernels would not build", dev->name );
        log_build_failure( dev );
        return 0;
    }
    return 1;
}

static cl_kernel take_kernel( struct ocl_device *dev, const char *name )
{
    cl_int    err;
    cl_kernel k = clCreateKernel( dev->prog, name, &err );

    if ( err != CL_SUCCESS )
    {
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: no kernel %s in the program (%d)",
                 dev->name, name, err );
        return NULL;
    }
    return k;
}

static void release_device( struct ocl_device *dev )
{
    for ( int i = 0; i < 16; i++ )
    {
        if ( dev->k64[i] != NULL ) clReleaseKernel( dev->k64[i] );
        if ( dev->k80[i] != NULL ) clReleaseKernel( dev->k80[i] );
        dev->k64[i] = dev->k80[i] = NULL;
    }
    if ( dev->kcheck   != NULL ) { clReleaseKernel( dev->kcheck );     dev->kcheck   = NULL; }
    if ( dev->m_header != NULL ) { clReleaseMemObject( dev->m_header ); dev->m_header = NULL; }
    if ( dev->m_hash   != NULL ) { clReleaseMemObject( dev->m_hash );   dev->m_hash   = NULL; }
    if ( dev->m_out    != NULL ) { clReleaseMemObject( dev->m_out );    dev->m_out    = NULL; }
    if ( dev->prog     != NULL ) { clReleaseProgram( dev->prog );       dev->prog     = NULL; }
    if ( dev->q        != NULL ) { clReleaseCommandQueue( dev->q );     dev->q        = NULL; }
    if ( dev->cx       != NULL ) { clReleaseContext( dev->cx );         dev->cx       = NULL; }
    dev->ready = 0;
}

/* The largest work group every kernel in the chain can take.
 *
 * Not the device's maximum: CL_KERNEL_WORK_GROUP_SIZE is per kernel and
 * accounts for how many registers that kernel needs, and these differ by an
 * order of magnitude. simd and echo are register-hungry enough that a group
 * sized for blake fails to launch at all, and the runtime picking its own size
 * when the local size is NULL is how that was discovered:
 * CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES, which names no kernel.
 *
 * Rounded down to a power of two so the global size is easy to keep a
 * multiple of it, and capped at 256 because nothing here benefits from a
 * larger group: there is no cross-item communication, only the per-group
 * lookup tables that echo, whirlpool, fugue, shavite and hamsi fill, and
 * those are filled with a strided loop that works at any size.
 */
static int settle_work_group( struct ocl_device *dev )
{
    size_t best = 256;

    for ( int i = 0; i < 16; i++ )
    {
        cl_kernel k[2] = { dev->k64[i], dev->k80[i] };

        for ( int j = 0; j < 2; j++ )
        {
            size_t n = 0;

            if ( clGetKernelWorkGroupInfo( k[j], dev->id,
                                           CL_KERNEL_WORK_GROUP_SIZE,
                                           sizeof n, &n, NULL ) != CL_SUCCESS
                 || n == 0 )
            {
                ocl_log( OCL_LOG_ERR, "OpenCL: %s: a kernel would not report "
                                      "its work group size", dev->name );
                return 0;
            }
            if ( n < best )
                best = n;
        }
    }

    /* Down to a power of two. */
    while ( ( best & ( best - 1 ) ) != 0 )
        best &= best - 1;

    if ( best < 32 )
    {
        /* Below a warp or a wavefront this would not be worth running, and it
         * would more likely mean something is wrong with the build than that
         * the card is small. */
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: the kernels allow only %zu work "
                              "items a group, which is too few to mine with",
                 dev->name, best );
        return 0;
    }

    dev->local = best;
    ocl_log( OCL_LOG_INFO, "OpenCL: %s: %zu work items a group",
             dev->name, dev->local );
    return 1;
}

static int prepare_device( struct ocl_device *dev )
{
    cl_int err;
    char   name[64];

    if ( dev->ready )
        return 1;
    if ( dev->broken )
        return 0;

    dev->cx = clCreateContext( NULL, 1, &dev->id, NULL, NULL, &err );
    if ( err == CL_SUCCESS )
        dev->q = clCreateCommandQueue( dev->cx, dev->id, 0, &err );
    if ( err != CL_SUCCESS )
    {
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: no context or queue (%d)", dev->name, err );
        goto fail;
    }

    ocl_log( OCL_LOG_INFO, "OpenCL: %s: compiling the kernels, which takes a "
                           "moment on first use", dev->name );
    if ( !build_program( dev ) )
        goto fail;

    for ( int i = 0; i < 16; i++ )
    {
        snprintf( name, sizeof name, "%s_64", s_algo[i] );
        dev->k64[i] = take_kernel( dev, name );
        snprintf( name, sizeof name, "%s_80", s_algo[i] );
        dev->k80[i] = take_kernel( dev, name );
        if ( dev->k64[i] == NULL || dev->k80[i] == NULL )
            goto fail;
    }
    dev->kcheck = take_kernel( dev, "x16_check" );
    if ( dev->kcheck == NULL )
        goto fail;

    if ( !settle_work_group( dev ) )
        goto fail;

    dev->m_header = clCreateBuffer( dev->cx, CL_MEM_READ_ONLY, 80, NULL, &err );
    if ( err == CL_SUCCESS )
        dev->m_hash = clCreateBuffer( dev->cx, CL_MEM_READ_WRITE,
                                      (size_t) dev->throughput * 64, NULL, &err );
    if ( err == CL_SUCCESS )
        dev->m_out = clCreateBuffer( dev->cx, CL_MEM_READ_WRITE,
                                     ( 1 + AVIAN_OCL_MAX_CANDIDATES ) * sizeof( cl_uint ),
                                     NULL, &err );
    if ( err != CL_SUCCESS )
    {
        /* The hash buffer is the large one, 64 MB at the default throughput.
         * A card that cannot spare it is a real possibility, and worth saying
         * plainly rather than reporting a bare error code. */
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: could not allocate buffers for %u "
                              "nonces a batch (%d)", dev->name, dev->throughput, err );
        goto fail;
    }

    ocl_log( OCL_LOG_INFO, "OpenCL: %s: ready, %u nonces a batch",
             dev->name, dev->throughput );
    dev->ready = 1;
    return 1;

fail:
    release_device( dev );
    dev->broken = 1;
    return 0;
}

AVIAN_CUDA_API int avian_cuda_abi_version( void )
{
    return AVIAN_CUDA_ABI_VERSION;
}

AVIAN_CUDA_API int avian_cuda_device_count( void )
{
    enumerate();
    return s_devices;
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
     * chain. Every kernel here is already compared against its sph
     * counterpart by avian-ocl-selftest, so a bisect would re-check what is
     * checked; what is new is the assembled chain and this driver, and an
     * error in either shows up as candidates that fail the miner's own
     * verification. Taken so the entry point exists. */
    (void) hash_n;
    (void) prehash;
}

/* Hex, because the order string is lower-case hex and a stray character must
 * not index past the kernel table. */
static int algo_from_hex( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
    return -1;
}

AVIAN_CUDA_API int avian_cuda_scan_batch( const avian_cuda_job *job,
                                          uint32_t *out_nonces, int max_out,
                                          uint32_t *throughput_used )
{
    struct ocl_device *dev;
    cl_uint   out[1 + AVIAN_OCL_MAX_CANDIDATES];
    cl_uint   zero = 0;
    cl_ulong  target;
    cl_uint   nonce_base;
    size_t    global;
    cl_int    err;
    int       algo[16];
    int       found = 0;

    if ( throughput_used != NULL )
        *throughput_used = 0;
    enumerate();
    if ( job == NULL || job->device < 0 || job->device >= s_devices )
        return -1;
    dev = &s_device[job->device];
    if ( !prepare_device( dev ) )
        return -1;

    /* The order decides which sixteen kernels run and in what sequence. A
     * malformed string is a miner bug rather than a device one, and running a
     * partial chain would give confidently wrong hashes, so it is refused. */
    for ( int i = 0; i < 16; i++ )
    {
        algo[i] = algo_from_hex( job->order[i] );
        if ( algo[i] < 0 )
        {
            ocl_log( OCL_LOG_ERR, "OpenCL: the order '%s' is not sixteen hex digits",
                     job->order );
            return -1;
        }
    }

    nonce_base = job->start_nonce;
    global     = dev->throughput;
    if ( job->max_nonce > job->start_nonce )
    {
        uint32_t left = job->max_nonce - job->start_nonce;

        if ( left < global )
            global = left;
    }
    /* OpenCL 1.2 has no non-uniform work groups, so the global size has to be
     * a multiple of the local size. Rounding down loses at most one group of
     * nonces from a batch, and the miner is told what was actually tried. */
    global -= global % dev->local;
    if ( global == 0 )
        return 0;

    target = ( (cl_ulong) job->target[7] << 32 ) | (cl_ulong) job->target[6];

    /* The header goes up once a batch; only the nonce differs per work item,
     * and the kernels derive that from their global id. */
    err = clEnqueueWriteBuffer( dev->q, dev->m_header, CL_FALSE, 0, 80,
                                job->data, 0, NULL, NULL );
    if ( err == CL_SUCCESS )
        err = clEnqueueWriteBuffer( dev->q, dev->m_out, CL_FALSE, 0,
                                    sizeof zero, &zero, 0, NULL, NULL );

    /* The first round takes the header, the other fifteen the hash in place.
     * Nothing waits between them: the queue is in order, so each kernel sees
     * the previous one's writes. */
    if ( err == CL_SUCCESS )
    {
        cl_kernel k = dev->k80[algo[0]];

        err = clSetKernelArg( k, 0, sizeof dev->m_header, &dev->m_header );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( k, 1, sizeof nonce_base, &nonce_base );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( k, 2, sizeof dev->m_hash, &dev->m_hash );
        if ( err == CL_SUCCESS )
            err = clEnqueueNDRangeKernel( dev->q, k, 1, NULL, &global, &dev->local,
                                          0, NULL, NULL );
    }
    for ( int i = 1; i < 16 && err == CL_SUCCESS; i++ )
    {
        cl_kernel k = dev->k64[algo[i]];

        err = clSetKernelArg( k, 0, sizeof dev->m_hash, &dev->m_hash );
        if ( err == CL_SUCCESS )
            err = clEnqueueNDRangeKernel( dev->q, k, 1, NULL, &global, &dev->local,
                                          0, NULL, NULL );
    }

    if ( err == CL_SUCCESS )
    {
        cl_uint max_cand = AVIAN_OCL_MAX_CANDIDATES;

        err = clSetKernelArg( dev->kcheck, 0, sizeof dev->m_hash, &dev->m_hash );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( dev->kcheck, 1, sizeof nonce_base, &nonce_base );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( dev->kcheck, 2, sizeof target, &target );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( dev->kcheck, 3, sizeof dev->m_out, &dev->m_out );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( dev->kcheck, 4, sizeof max_cand, &max_cand );
        if ( err == CL_SUCCESS )
            err = clEnqueueNDRangeKernel( dev->q, dev->kcheck, 1, NULL,
                                          &global, &dev->local,
                                          0, NULL, NULL );
    }

    if ( err == CL_SUCCESS )
        err = clEnqueueReadBuffer( dev->q, dev->m_out, CL_TRUE, 0, sizeof out,
                                   out, 0, NULL, NULL );
    if ( err != CL_SUCCESS )
    {
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: the batch failed (%d)", dev->name, err );
        return -1;
    }

    /* Every nonce in the range was tried whether or not it qualified, and the
     * miner counts hashes from this rather than from the candidates. */
    if ( throughput_used != NULL )
        *throughput_used = (uint32_t) global;

    if ( out[0] > AVIAN_OCL_MAX_CANDIDATES )
    {
        /* The kernel increments the count past the buffer on purpose, so this
         * can say how many were lost rather than pretend it was enough. */
        ocl_log( OCL_LOG_WARNING, "OpenCL: %s: %u candidates in one batch, "
                                  "keeping %d", dev->name, out[0],
                 AVIAN_OCL_MAX_CANDIDATES );
        out[0] = AVIAN_OCL_MAX_CANDIDATES;
    }
    for ( cl_uint i = 0; i < out[0] && found < max_out; i++ )
        out_nonces[found++] = out[1 + i];

    return found;
}

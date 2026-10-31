/* Is every OpenCL kernel byte-exact against the CPU implementation it
 * replaces?
 *
 *   ./avian-ocl-selftest            # every kernel, on the first OpenCL GPU
 *   ./avian-ocl-selftest jh512_64   # one of them
 *
 * A separate binary rather than part of --avian-selftest, because it has to
 * run before there is a batch loop to hang a diagnostic off: this compares
 * one round function against one sph function, which is the smallest thing
 * that can be wrong. Once avian_cuda_scan_batch exists, the stage bisect
 * through avian_cuda_set_diagnostics covers the assembled chain, and the two
 * answer different questions.
 *
 * Exits non-zero on any mismatch, so a build or a CI job can gate on it.
 *
 * It tests the embedded sources, the same strings the backend compiles, not
 * the .cl files on disk. A test that read the files could pass while the
 * library shipped something else.
 */

#define CL_TARGET_OPENCL_VERSION 120

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CL/cl.h>

#include "cuda/ocl/avian-ocl-kernels.h"

#include "algo/jh/sph_jh.h"
#include "algo/groestl/sph_groestl.h"
#include "algo/shavite/sph_shavite.h"

/* Enough lanes that a bug depending on the work item index shows up, and few
 * enough that a CPU OpenCL device finishes quickly. */
#define LANES 64

typedef void (*cpu_hash_fn)( void *out, const void *in );

static void cpu_jh512( void *out, const void *in )
{
    sph_jh512_context c;

    sph_jh512_init( &c );
    sph_jh512( &c, in, 64 );
    sph_jh512_close( &c, out );
}

static void cpu_groestl512( void *out, const void *in )
{
    sph_groestl512_context c;

    sph_groestl512_init( &c );
    sph_groestl512( &c, in, 64 );
    sph_groestl512_close( &c, out );
}

/* Whichever sph_shavite.h selected: the AES-NI implementation where the host
 * has those instructions, the software one otherwise. Both are linked, and
 * either is a valid reference because they must agree with each other. */
static void cpu_shavite512( void *out, const void *in )
{
    sph_shavite512_context c;

    sph_shavite512_init( &c );
    sph_shavite512( &c, in, 64 );
    sph_shavite512_close( &c, out );
}

/* One row per kernel, naming the sph function it must agree with. The entry
 * point is the key into avian_ocl_kernels, so a kernel added to the .cl
 * directory without a row here is reported rather than silently untested. */
static const struct
{
    const char  *entry;
    cpu_hash_fn  cpu;
} s_case[] = {
    { "jh512_64",      cpu_jh512 },
    { "groestl512_64", cpu_groestl512 },
    { "shavite512_64", cpu_shavite512 },
};

#define CASE_COUNT ( sizeof s_case / sizeof s_case[0] )

static const char *kernel_source( const char *entry )
{
    for ( size_t i = 0; i < AVIAN_OCL_KERNEL_COUNT; i++ )
        if ( strcmp( avian_ocl_kernels[i].entry, entry ) == 0 )
            return avian_ocl_kernels[i].source;
    return NULL;
}

static void fill( unsigned char buf[LANES][64] )
{
    unsigned int seed = 1;

    /* The edges first, because all-zero and all-ones are where a wrong
     * padding byte or length constant stops cancelling out. */
    memset( buf[0], 0x00, 64 );
    memset( buf[1], 0xff, 64 );
    for ( int l = 2; l < LANES; l++ )
        for ( int i = 0; i < 64; i++ )
        {
            seed = seed * 1103515245u + 12345u;
            buf[l][i] = (unsigned char) ( seed >> 16 );
        }
}

static cl_device_id first_gpu( char *name, size_t name_len )
{
    cl_platform_id platform[8];
    cl_uint        platforms = 0;

    if ( clGetPlatformIDs( 8, platform, &platforms ) != CL_SUCCESS )
        return NULL;
    for ( cl_uint p = 0; p < platforms; p++ )
    {
        cl_device_id dev = NULL;
        cl_uint      n   = 0;

        if ( clGetDeviceIDs( platform[p], CL_DEVICE_TYPE_GPU, 1, &dev, &n )
             != CL_SUCCESS || n == 0 )
            continue;
        clGetDeviceInfo( dev, CL_DEVICE_NAME, name_len, name, NULL );
        return dev;
    }
    return NULL;
}

static void print_build_log( cl_program pr, cl_device_id dev )
{
    size_t n = 0;
    char  *log;

    clGetProgramBuildInfo( pr, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &n );
    if ( n <= 1 )
        return;
    log = malloc( n + 1 );
    if ( log == NULL )
        return;
    clGetProgramBuildInfo( pr, dev, CL_PROGRAM_BUILD_LOG, n, log, NULL );
    log[n] = '\0';
    printf( "%s\n", log );
    free( log );
}

/* 0 on agreement, 1 on a mismatch, 2 when the kernel would not build. */
static int run_one( cl_context cx, cl_command_queue q, cl_device_id dev,
                    const char *entry, cpu_hash_fn cpu )
{
    static unsigned char in[LANES][64], want[LANES][64], got[LANES][64];
    const char          *src = kernel_source( entry );
    size_t               len;
    cl_program           pr;
    cl_kernel            k;
    cl_mem               buf;
    cl_int               err;
    size_t               global = LANES;
    int                  bad = 0;

    if ( src == NULL )
    {
        printf( "  %-14s no embedded source for this entry point\n", entry );
        return 2;
    }
    fill( in );
    for ( int l = 0; l < LANES; l++ )
        cpu( want[l], in[l] );

    len = strlen( src );
    pr  = clCreateProgramWithSource( cx, 1, &src, &len, &err );
    if ( err != CL_SUCCESS )
    {
        printf( "  %-14s clCreateProgramWithSource failed (%d)\n", entry, err );
        return 2;
    }
    if ( clBuildProgram( pr, 1, &dev, "", NULL, NULL ) != CL_SUCCESS )
    {
        printf( "  %-14s will not build:\n", entry );
        print_build_log( pr, dev );
        clReleaseProgram( pr );
        return 2;
    }

    k = clCreateKernel( pr, entry, &err );
    if ( err != CL_SUCCESS )
    {
        /* The log matters here too, and this is where it was missing. A build
         * can return success while the entry point is absent, and then only
         * the warnings say why: a macro that renamed the function, or a
         * preprocessor branch that compiled it out. */
        printf( "  %-14s built, but has no such kernel in it (%d)\n", entry, err );
        print_build_log( pr, dev );
        clReleaseProgram( pr );
        return 2;
    }

    buf = clCreateBuffer( cx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                          sizeof in, in, &err );
    if ( err == CL_SUCCESS )
        err = clSetKernelArg( k, 0, sizeof buf, &buf );
    if ( err == CL_SUCCESS )
        err = clEnqueueNDRangeKernel( q, k, 1, NULL, &global, NULL, 0, NULL, NULL );
    if ( err == CL_SUCCESS )
        err = clFinish( q );
    if ( err == CL_SUCCESS )
        err = clEnqueueReadBuffer( q, buf, CL_TRUE, 0, sizeof got, got,
                                   0, NULL, NULL );
    if ( err != CL_SUCCESS )
    {
        printf( "  %-14s the device refused the work (%d)\n", entry, err );
        clReleaseMemObject( buf );
        clReleaseKernel( k );
        clReleaseProgram( pr );
        return 2;
    }

    for ( int l = 0; l < LANES; l++ )
        if ( memcmp( got[l], want[l], 64 ) != 0 )
        {
            /* The first two in full, since a byte-swap or a one-word shift is
             * recognisable on sight and a count is not. */
            if ( bad < 2 )
            {
                printf( "  %-14s lane %d differs\n    cpu ", entry, l );
                for ( int i = 0; i < 64; i++ )
                    printf( "%02x", want[l][i] );
                printf( "\n    ocl " );
                for ( int i = 0; i < 64; i++ )
                    printf( "%02x", got[l][i] );
                printf( "\n" );
            }
            bad++;
        }

    clReleaseMemObject( buf );
    clReleaseKernel( k );
    clReleaseProgram( pr );

    if ( bad == 0 )
        printf( "  %-14s %d lanes, 0 differing\n", entry, LANES );
    else
        printf( "  %-14s %d lanes, %d differing\n", entry, LANES, bad );
    return bad != 0;
}

int main( int argc, char **argv )
{
    const char      *only = ( argc > 1 ) ? argv[1] : NULL;
    char             name[256] = { 0 };
    cl_device_id     dev;
    cl_context       cx;
    cl_command_queue q;
    cl_int           err;
    int              failed = 0, ran = 0;

    dev = first_gpu( name, sizeof name );
    if ( dev == NULL )
    {
        /* Not a failure. A machine with no OpenCL GPU cannot run this, and
         * saying so beats exiting non-zero and looking like a broken kernel.
         * POCL offers a CPU device for exactly this case; doc/building.md
         * says how. */
        printf( "no OpenCL GPU here, so nothing was checked\n" );
        return 0;
    }
    printf( "device: %s\n", name );

    cx = clCreateContext( NULL, 1, &dev, NULL, NULL, &err );
    if ( err != CL_SUCCESS )
    {
        printf( "clCreateContext failed (%d)\n", err );
        return 1;
    }
    q = clCreateCommandQueue( cx, dev, 0, &err );
    if ( err != CL_SUCCESS )
    {
        printf( "clCreateCommandQueue failed (%d)\n", err );
        clReleaseContext( cx );
        return 1;
    }

    for ( size_t i = 0; i < CASE_COUNT; i++ )
    {
        if ( only != NULL && strcmp( only, s_case[i].entry ) != 0 )
            continue;
        ran++;
        if ( run_one( cx, q, dev, s_case[i].entry, s_case[i].cpu ) != 0 )
            failed++;
    }

    /* A kernel with no row above is untested, which is worth a word: the
     * silent version of this is a kernel nobody ever compared. */
    for ( size_t j = 0; j < AVIAN_OCL_KERNEL_COUNT; j++ )
    {
        int known = 0;

        for ( size_t i = 0; i < CASE_COUNT; i++ )
            if ( strcmp( avian_ocl_kernels[j].entry, s_case[i].entry ) == 0 )
                known = 1;
        if ( !known )
            printf( "  %-14s embedded but has no row in this test\n",
                    avian_ocl_kernels[j].entry );
    }

    clReleaseCommandQueue( q );
    clReleaseContext( cx );

    if ( only != NULL && ran == 0 )
    {
        printf( "no kernel called %s\n", only );
        return 1;
    }
    printf( "%d checked, %d failed\n", ran, failed );
    return failed != 0;
}

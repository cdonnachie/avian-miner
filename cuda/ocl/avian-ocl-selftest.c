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

#include "algo/blake/sph_blake.h"
#include "algo/jh/sph_jh.h"
#include "algo/groestl/sph_groestl.h"
#include "algo/shavite/sph_shavite.h"
#include "algo/bmw/sph_bmw.h"
#include "algo/skein/sph_skein.h"
#include "algo/keccak/sph_keccak.h"
#include "algo/simd/sph_simd.h"
#include "algo/echo/sph_echo.h"
#include "algo/shabal/sph_shabal.h"
#include "algo/hamsi/sph_hamsi.h"
#include "algo/luffa/sph_luffa.h"
#include "algo/cubehash/sph_cubehash.h"
#include "algo/fugue/sph_fugue.h"
#include "algo/whirlpool/sph_whirlpool.h"
#include "algo/sha/sph_sha2.h"

/* Enough lanes that a bug depending on the work item index shows up, and few
 * enough that a CPU OpenCL device finishes quickly. */
#define LANES 64

/* sph_keccak.c reads this to pick its padding byte, and the definition lives
 * in keccak-gate.c, which a test binary has no business linking: that file
 * pulls in the whole algo_gate machinery.
 *
 * 1, not 6, and the difference matters. 1 is classic Keccak and 6 is SHA3,
 * and the only thing in this tree that sets 6 is register_sha3d_algo, which
 * is an algorithm this miner no longer builds. X16RT therefore hashes with
 * classic Keccak, which is also what sgminer's x15 kernel does, so the two
 * agree. Were that wrong, this would be comparing the GPU against the wrong
 * reference and calling it a pass.
 */
int hard_coded_eb = 1;

typedef void (*cpu_hash_fn)( void *out, const void *in, size_t len );

/* Every sph function has the same three-call shape, so one macro beats
 * sixteen copies of it.
 *
 * The length is a parameter because X16RT's first round hashes the 80-byte
 * header and every round after it hashes 64 bytes, so each algorithm needs
 * both and the reference is the same function called differently. */
#define CPU_REFERENCE( name )                                           \
    static void cpu_##name( void *out, const void *in, size_t len )     \
    {                                                                   \
        sph_##name##_context c;                                         \
                                                                        \
        sph_##name##_init( &c );                                        \
        sph_##name( &c, in, len );                                      \
        sph_##name##_close( &c, out );                                  \
    }

CPU_REFERENCE( blake512 )
CPU_REFERENCE( jh512 )
CPU_REFERENCE( groestl512 )
CPU_REFERENCE( bmw512 )
CPU_REFERENCE( skein512 )
CPU_REFERENCE( keccak512 )
CPU_REFERENCE( simd512 )
CPU_REFERENCE( echo512 )
CPU_REFERENCE( shabal512 )
CPU_REFERENCE( hamsi512 )
CPU_REFERENCE( luffa512 )
CPU_REFERENCE( cubehash512 )
CPU_REFERENCE( fugue512 )
CPU_REFERENCE( sha512 )

/* Not through the macro: sph_shavite.h redirects sph_shavite512_* to the
 * AES-NI implementation where the host has those instructions and to the
 * software one otherwise, but names the context type sph_shavite512_context
 * either way, so the macro's sph_shavite512_context spelling would be wrong
 * in neither case and right by accident. Spelled out so that is visible. */
static void cpu_shavite512( void *out, const void *in, size_t len )
{
    sph_shavite512_context c;

    sph_shavite512_init( &c );
    sph_shavite512( &c, in, len );
    sph_shavite512_close( &c, out );
}

/* Not through the macro either: Whirlpool is 512 bits by definition, so sph
 * spells it without a size. sph_whirlpool is the final ISO revision, which is
 * the one X16R uses; sph_whirlpool0 and sph_whirlpool1 are the superseded
 * drafts and would quietly produce a different hash. */
static void cpu_whirlpool( void *out, const void *in, size_t len )
{
    sph_whirlpool_context c;

    sph_whirlpool_init( &c );
    sph_whirlpool( &c, in, len );
    sph_whirlpool_close( &c, out );
}

/* One row per kernel, naming the sph function it must agree with. The entry
 * point is the key into avian_ocl_kernels, so a kernel added to the .cl
 * directory without a row here is reported rather than silently untested. */
static const struct
{
    const char  *entry;
    cpu_hash_fn  cpu;
    int          in_len;    /* 64 for a round, 80 for a first round */
} s_case[] = {
    { "blake512_64",    cpu_blake512, 64 },
    { "jh512_64",       cpu_jh512, 64 },
    { "groestl512_64",  cpu_groestl512, 64 },
    { "shavite512_64",  cpu_shavite512, 64 },
    { "bmw512_64",      cpu_bmw512, 64 },
    { "skein512_64",    cpu_skein512, 64 },
    { "keccak512_64",   cpu_keccak512, 64 },
    { "simd512_64",     cpu_simd512, 64 },
    { "echo512_64",     cpu_echo512, 64 },
    { "shabal512_64",   cpu_shabal512, 64 },
    { "luffa512_64",    cpu_luffa512, 64 },
    { "cubehash512_64", cpu_cubehash512, 64 },
    { "fugue512_64",    cpu_fugue512, 64 },
    { "whirlpool512_64", cpu_whirlpool, 64 },
    { "sha512_64",      cpu_sha512, 64 },
    { "hamsi512_64",    cpu_hamsi512, 64 },

    /* The first rounds: the 80-byte header in, 64 bytes out, with the
     * nonce taken from the work item rather than the header. */
    { "blake512_80",    cpu_blake512, 80 },
    { "sha512_80",      cpu_sha512, 80 },
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

/* The byte conversions a kernel's boundary might need.
 *
 * sgminer's chain files hold their inter-stage hash in whatever form the next
 * stage wants, so there is no one convention: luffa is symmetric, cubehash
 * loads byte-swapped and stores plain, fugue does the reverse. A ported
 * kernel therefore needs its own conversion at the boundary, and guessing
 * which is how a kernel comes to compile and hash wrongly.
 *
 * --explore applies each of these to the input before the kernel and to the
 * output after it, and reports the pairs that reproduce the sph answer. The
 * one that matches is then built into the kernel.
 */
enum conv { CONV_NONE, CONV_SWAP4, CONV_SWAP8, CONV_WORDS, CONV_COUNT };

static const char *conv_name[CONV_COUNT] = {
    "none", "swap4", "swap8", "words"
};

static void convert( unsigned char *dst, const unsigned char *src, enum conv c )
{
    switch ( c )
    {
    case CONV_SWAP4:        /* byte-reverse each 32-bit word */
        for ( int w = 0; w < 16; w++ )
            for ( int b = 0; b < 4; b++ )
                dst[w * 4 + b] = src[w * 4 + 3 - b];
        break;
    case CONV_SWAP8:        /* byte-reverse each 64-bit word */
        for ( int w = 0; w < 8; w++ )
            for ( int b = 0; b < 8; b++ )
                dst[w * 8 + b] = src[w * 8 + 7 - b];
        break;
    case CONV_WORDS:        /* swap the 32-bit halves of each 64-bit word */
        for ( int w = 0; w < 8; w++ )
        {
            memcpy( dst + w * 8,     src + w * 8 + 4, 4 );
            memcpy( dst + w * 8 + 4, src + w * 8,     4 );
        }
        break;
    case CONV_NONE:
    default:
        memcpy( dst, src, 64 );
        break;
    }
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

/* The nonce every 80-byte case starts from. Arbitrary, but neither zero nor
 * small, so a kernel that ignores the nonce or truncates it is caught. */
#define NONCE_BASE 0x9E3779B9u

/* 0 on agreement, 1 on a mismatch, 2 when the kernel would not build. */
static int run_one( cl_context cx, cl_command_queue q, cl_device_id dev,
                    const char *entry, cpu_hash_fn cpu, int in_len )
{
    static unsigned char in[LANES][64], want[LANES][64], got[LANES][64];
    static unsigned char header[80], hdr_lane[LANES][80];
    const char          *src = kernel_source( entry );
    size_t               len;
    cl_program           pr;
    cl_kernel            k;
    cl_mem               buf, hbuf = NULL;
    cl_int               err;
    size_t               global = LANES;
    int                  bad = 0;

    if ( src == NULL )
    {
        printf( "  %-14s no embedded source for this entry point\n", entry );
        return 2;
    }
    if ( in_len == 80 )
    {
        /* One header shared by every lane, with the nonce at bytes 76 to 79
         * differing per lane, which is what the kernel derives from its
         * global id. The reference patches the same four bytes in the same
         * place and hashes 80 bytes, so what is compared is the hash and not
         * anyone's opinion about byte order. */
        unsigned int seed = 0xC0FFEE;

        for ( int i = 0; i < 80; i++ )
        {
            seed = seed * 1103515245u + 12345u;
            header[i] = (unsigned char) ( seed >> 16 );
        }
        for ( int l = 0; l < LANES; l++ )
        {
            unsigned int nonce = NONCE_BASE + (unsigned int) l;

            memcpy( hdr_lane[l], header, 80 );
            memcpy( hdr_lane[l] + 76, &nonce, 4 );
            cpu( want[l], hdr_lane[l], 80 );
        }
    }
    else
    {
        fill( in );
        for ( int l = 0; l < LANES; l++ )
            cpu( want[l], in[l], 64 );
    }

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

    if ( in_len == 80 )
    {
        /* (header, nonce_base, hashes). The header goes up once rather than
         * per lane, because that is how the backend will hand it over: a
         * kernel wanting 80 bytes per lane would be the wrong shape for
         * mining, where only the nonce differs between work items. */
        cl_uint base = NONCE_BASE;

        hbuf = clCreateBuffer( cx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                               80, header, &err );
        buf = ( err == CL_SUCCESS )
            ? clCreateBuffer( cx, CL_MEM_WRITE_ONLY, sizeof got, NULL, &err )
            : NULL;
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( k, 0, sizeof hbuf, &hbuf );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( k, 1, sizeof base, &base );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( k, 2, sizeof buf, &buf );
    }
    else
    {
        buf = clCreateBuffer( cx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                              sizeof in, in, &err );
        if ( err == CL_SUCCESS )
            err = clSetKernelArg( k, 0, sizeof buf, &buf );
    }
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
        if ( hbuf != NULL ) clReleaseMemObject( hbuf );
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

    if ( hbuf != NULL ) clReleaseMemObject( hbuf );
    clReleaseMemObject( buf );
    clReleaseKernel( k );
    clReleaseProgram( pr );

    if ( bad == 0 )
        printf( "  %-14s %d lanes, 0 differing\n", entry, LANES );
    else
        printf( "  %-14s %d lanes, %d differing\n", entry, LANES, bad );
    return bad != 0;
}

/* Runs the kernel once and returns its output for the given input, or
 * non-zero on any OpenCL failure. Separate from run_one because explore
 * needs the raw output rather than a verdict. */
static int run_raw( cl_context cx, cl_command_queue q, cl_device_id dev,
                    const char *entry,
                    const unsigned char in[LANES][64],
                    unsigned char out[LANES][64] )
{
    const char *src = kernel_source( entry );
    size_t      len;
    cl_program  pr;
    cl_kernel   k;
    cl_mem      buf;
    cl_int      err;
    size_t      global = LANES;

    if ( src == NULL )
        return 1;
    len = strlen( src );
    pr  = clCreateProgramWithSource( cx, 1, &src, &len, &err );
    if ( err != CL_SUCCESS )
        return 1;
    if ( clBuildProgram( pr, 1, &dev, "", NULL, NULL ) != CL_SUCCESS )
    {
        print_build_log( pr, dev );
        clReleaseProgram( pr );
        return 1;
    }
    k = clCreateKernel( pr, entry, &err );
    if ( err != CL_SUCCESS )
    {
        clReleaseProgram( pr );
        return 1;
    }
    buf = clCreateBuffer( cx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                          (size_t) LANES * 64, (void *) in, &err );
    if ( err == CL_SUCCESS )
        err = clSetKernelArg( k, 0, sizeof buf, &buf );
    if ( err == CL_SUCCESS )
        err = clEnqueueNDRangeKernel( q, k, 1, NULL, &global, NULL, 0, NULL, NULL );
    if ( err == CL_SUCCESS )
        err = clFinish( q );
    if ( err == CL_SUCCESS )
        err = clEnqueueReadBuffer( q, buf, CL_TRUE, 0, (size_t) LANES * 64,
                                   out, 0, NULL, NULL );
    clReleaseMemObject( buf );
    clReleaseKernel( k );
    clReleaseProgram( pr );
    return err != CL_SUCCESS;
}

/* Which pre/post conversion pair makes this kernel agree with its sph
 * function? Tries them all and says. Reports every match, because more than
 * one hitting is itself information: it means the test inputs are not
 * distinguishing them and need to be harder. */
static int explore( cl_context cx, cl_command_queue q, cl_device_id dev,
                    const char *entry, cpu_hash_fn cpu )
{
    static unsigned char in[LANES][64], want[LANES][64];
    static unsigned char fed[LANES][64], got[LANES][64];
    int matches = 0;

    fill( in );
    for ( int l = 0; l < LANES; l++ )
        cpu( want[l], in[l], 64 );

    printf( "exploring %s\n", entry );
    for ( int pre = 0; pre < CONV_COUNT; pre++ )
    {
        for ( int l = 0; l < LANES; l++ )
            convert( fed[l], in[l], (enum conv) pre );
        if ( run_raw( cx, q, dev, entry, fed, got ) != 0 )
        {
            printf( "  pre=%-6s the kernel would not run\n", conv_name[pre] );
            return -1;
        }
        for ( int post = 0; post < CONV_COUNT; post++ )
        {
            unsigned char cand[64];
            int bad = 0;

            for ( int l = 0; l < LANES && !bad; l++ )
            {
                convert( cand, got[l], (enum conv) post );
                if ( memcmp( cand, want[l], 64 ) != 0 )
                    bad = 1;
            }
            printf( "  pre=%-6s post=%-6s %s\n", conv_name[pre],
                    conv_name[post], bad ? "differ" : "MATCH" );
            if ( !bad )
                matches++;
        }
    }
    if ( matches == 0 )
        printf( "  nothing matched: the difference is not a byte permutation, "
                "so the body itself needs looking at\n" );
    return matches;
}

int main( int argc, char **argv )
{
    const char      *only = ( argc > 1 ) ? argv[1] : NULL;
    int              exploring = 0;
    char             name[256] = { 0 };
    cl_device_id     dev;
    cl_context       cx;
    cl_command_queue q;
    cl_int           err;
    int              failed = 0, ran = 0;

    if ( only != NULL && strcmp( only, "--explore" ) == 0 )
    {
        exploring = 1;
        only = ( argc > 2 ) ? argv[2] : NULL;
        if ( only == NULL )
        {
            printf( "usage: avian-ocl-selftest --explore <entry>\n" );
            return 1;
        }
    }

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
        if ( exploring )
        {
            if ( explore( cx, q, dev, s_case[i].entry, s_case[i].cpu ) <= 0 )
                failed++;
        }
        else if ( run_one( cx, q, dev, s_case[i].entry, s_case[i].cpu,
                              s_case[i].in_len ) != 0 )
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

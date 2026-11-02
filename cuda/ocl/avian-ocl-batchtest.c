/* Does the assembled chain actually scan?
 *
 *   ./avian-ocl-batchtest            # a few orders
 *   ./avian-ocl-batchtest 0123456789abcdef   # one specific order
 *
 * avian-ocl-selftest checks each kernel against its sph counterpart. This
 * checks the rest: that the backend library loads, that the sixteen kernels
 * run in the order a job asks for, that the nonce reaches the first round,
 * and that the candidates coming back are real.
 *
 * It verifies them the way the miner does, which is the point. The ABI makes
 * the caller re-hash every candidate on the CPU before submitting, so this
 * runs the same X16RT chain here with sph and checks the device's answer
 * against it. A wrong chain order, a dropped round, a nonce that never
 * reached the kernel: all of them show up as candidates that do not verify.
 *
 * The backend is loaded rather than linked, so what is tested is the library
 * the miner will load, and it is loaded the way the miner loads it. On an AMD
 * card this is also the one check that does not need the GPU to be idle: it
 * says whether the chain is right, whatever the clocks are doing.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cuda/avian-cuda-api.h"
/* For AVIAN_GPU_OPENCL_LIB_NAME alone, so the file this looks for cannot
 * drift from the one the miner looks for. */
#include "cuda/avian-gpu-backend.h"

#ifdef _WIN32
#include <windows.h>
/* A bare name, because Windows searches the directory holding the executable
 * and that is where make leaves both of these. */
#define OCL_TEST_LIB            AVIAN_GPU_OPENCL_LIB_NAME
#define OCL_TEST_OPEN( p )      ( (void *) LoadLibraryA( p ) )
#define OCL_TEST_SYM( h, n )    ( (void *) GetProcAddress( (HMODULE) (h), n ) )
#else
#include <dlfcn.h>
/* "./" because dlopen on a bare name searches the loader's own paths and not
 * the working directory. */
#define OCL_TEST_LIB            "./" AVIAN_GPU_OPENCL_LIB_NAME
#define OCL_TEST_OPEN( p )      dlopen( p, RTLD_NOW )
#define OCL_TEST_SYM( h, n )    dlsym( h, n )
#endif

#include "algo/blake/sph_blake.h"
#include "algo/bmw/sph_bmw.h"
#include "algo/groestl/sph_groestl.h"
#include "algo/jh/sph_jh.h"
#include "algo/keccak/sph_keccak.h"
#include "algo/skein/sph_skein.h"
#include "algo/luffa/sph_luffa.h"
#include "algo/cubehash/sph_cubehash.h"
#include "algo/shavite/sph_shavite.h"
#include "algo/simd/sph_simd.h"
#include "algo/echo/sph_echo.h"
#include "algo/hamsi/sph_hamsi.h"
#include "algo/fugue/sph_fugue.h"
#include "algo/shabal/sph_shabal.h"
#include "algo/whirlpool/sph_whirlpool.h"
#include "algo/sha/sph_sha2.h"

/* sph_keccak.c reads this for its padding byte, and the definition lives in
 * keccak-gate.c which a test binary should not link. 1 is classic Keccak,
 * which is what X16RT uses; see avian-ocl-selftest.c. */
int hard_coded_eb = 1;

#define CPU_ROUND( name )                                               \
    static void cpu_##name( void *out, const void *in, size_t len )     \
    {                                                                   \
        sph_##name##_context c;                                         \
                                                                        \
        sph_##name##_init( &c );                                        \
        sph_##name( &c, in, len );                                      \
        sph_##name##_close( &c, out );                                  \
    }

CPU_ROUND( blake512 )
CPU_ROUND( bmw512 )
CPU_ROUND( groestl512 )
CPU_ROUND( jh512 )
CPU_ROUND( keccak512 )
CPU_ROUND( skein512 )
CPU_ROUND( luffa512 )
CPU_ROUND( cubehash512 )
CPU_ROUND( shavite512 )
CPU_ROUND( simd512 )
CPU_ROUND( echo512 )
CPU_ROUND( hamsi512 )
CPU_ROUND( fugue512 )
CPU_ROUND( shabal512 )
CPU_ROUND( sha512 )

static void cpu_whirlpool512( void *out, const void *in, size_t len )
{
    sph_whirlpool_context c;

    sph_whirlpool_init( &c );
    sph_whirlpool( &c, in, len );
    sph_whirlpool_close( &c, out );
}

typedef void (*round_fn)( void *out, const void *in, size_t len );

/* Indexed as x16r_Algo numbers them, which is what an order string indexes. */
static const round_fn s_round[16] = {
    cpu_blake512, cpu_bmw512, cpu_groestl512, cpu_jh512,
    cpu_keccak512, cpu_skein512, cpu_luffa512, cpu_cubehash512,
    cpu_shavite512, cpu_simd512, cpu_echo512, cpu_hamsi512,
    cpu_fugue512, cpu_shabal512, cpu_whirlpool512, cpu_sha512
};

static int algo_from_hex( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
    return -1;
}

/* The whole chain on the CPU: eighty bytes into the first round, sixty-four
 * through the other fifteen. */
static int cpu_chain( unsigned char out[64], const unsigned char header[80],
                      const char *order )
{
    unsigned char buf[64];

    for ( int i = 0; i < 16; i++ )
    {
        int a = algo_from_hex( order[i] );

        if ( a < 0 )
            return 0;
        if ( i == 0 )
            s_round[a]( buf, header, 80 );
        else
            s_round[a]( buf, buf, 64 );
    }
    memcpy( out, buf, 64 );
    return 1;
}

/* The miner's be32enc, spelled out rather than linked: this test builds
 * against sphlib alone and pulling in the miner's util would bring the whole
 * algo gate with it. */
static void be32enc_local( uint32_t *out, uint32_t x )
{
    unsigned char *p = (unsigned char *) out;

    p[0] = (unsigned char) ( x >> 24 );
    p[1] = (unsigned char) ( x >> 16 );
    p[2] = (unsigned char) ( x >> 8 );
    p[3] = (unsigned char) x;
}

static uint64_t top64( const unsigned char h[64] )
{
    uint64_t v;

    /* The hash is little-endian, so its most significant eight bytes are
     * 24 to 31, which is the word the device compares. */
    memcpy( &v, h + 24, 8 );
    return v;
}

struct backend
{
    int         (*abi)( void );
    int         (*count)( void );
    const char *(*name)( int );
    void        (*setlog)( avian_cuda_log_fn );
    int         (*scan)( const avian_cuda_job *, uint32_t *, int, uint32_t * );
};

static void logger( int level, const char *msg )
{
    printf( "  [%d] %s\n", level, msg );
}

static int run_order( struct backend *b, const char *order, int device )
{
    avian_cuda_job job;
    unsigned char  header[80], want[64];
    uint32_t       edata[20];
    uint32_t       cand[64];
    uint32_t       used = 0;
    unsigned int   seed = 0x5EED;
    int            n, bad = 0;

    memset( &job, 0, sizeof job );
    for ( int i = 0; i < 80; i++ )
    {
        seed = seed * 1103515245u + 12345u;
        header[i] = (unsigned char) ( seed >> 16 );
    }
    memcpy( job.data, header, 80 );

    /* An easy target so a 2^20 batch finds a handful: about one nonce in
     * 65536 clears it, which is enough to check and few enough to print.
     * target[7] is the most significant word, as cpuminer stores it. */
    job.target[7] = 0x0000FFFFu;
    job.target[6] = 0xFFFFFFFFu;

    memcpy( job.order, order, 16 );
    job.order[16]  = '\0';
    job.start_nonce = 0x10000000u;
    job.max_nonce   = 0xFFFFFFFFu;
    job.device      = device;

    n = b->scan( &job, cand, (int) ( sizeof cand / sizeof cand[0] ), &used );
    if ( n < 0 )
    {
        printf( "  %s  the batch failed\n", order );
        return 1;
    }
    if ( used == 0 )
    {
        printf( "  %s  reported no nonces tried\n", order );
        return 1;
    }

    for ( int i = 0; i < n; i++ )
    {
        uint64_t  t = ( (uint64_t) job.target[7] << 32 ) | job.target[6];
        uint32_t  nonce = cand[i];

        /* The miner's own convention, not this test's.
         *
         * This used to patch the raw nonce into the raw header and hash
         * that, which checked the device against the way this file happened
         * to build a header. The device agreed, for years, while the miner
         * rejected every candidate it was given: the ABI says job.data is
         * the header in byte-swapped words, so what has to be hashed is
         * be32enc of each word with be32enc of the nonce at word 19, which
         * is what algo/x16/x16rt-cuda-host.c builds before calling
         * x16r_hash. A test that verifies against anything else cannot see
         * a backend whose header convention is wrong, which is exactly the
         * bug that got through. */
        for ( int k = 0; k < 19; k++ )
            be32enc_local( &edata[k], job.data[k] );
        be32enc_local( &edata[19], nonce );
        if ( !cpu_chain( want, (const unsigned char *) edata, order ) )
        {
            printf( "  %s  the order is not hex\n", order );
            return 1;
        }
        if ( top64( want ) > t )
        {
            printf( "  %s  nonce %08x does not verify: cpu top64 %016llx > "
                    "target %016llx\n", order, nonce,
                    (unsigned long long) top64( want ),
                    (unsigned long long) t );
            bad++;
        }
    }

    printf( "  %s  %u nonces, %d candidates, %d not verifying\n",
            order, used, n, bad );
    return bad != 0;
}

int main( int argc, char **argv )
{
    void           *h;
    struct backend  b;
    int             failed = 0;
    int             device = 0, listing = 0, devices;
    const char     *order = NULL;
    /* A few orders rather than one: all sixteen algorithms as the first
     * round across the set, since that is the part a 64-byte-only chain
     * would never exercise. */
    /* Sixteen rotations of one permutation, plus three unrelated ones.
     *
     * The rotations put every algorithm first in turn, and the first round
     * is the only one with a kernel of its own: it takes the 80-byte header
     * and injects the nonce, where the other fifteen take 64 bytes in
     * place. So a header or nonce convention wrong in one of those sixteen
     * kernels shows up here and nowhere else. The old list covered three of
     * them. */
    static const char *const orders[] = {
        "0123456789abcdef", "123456789abcdef0",
        "23456789abcdef01", "3456789abcdef012",
        "456789abcdef0123", "56789abcdef01234",
        "6789abcdef012345", "789abcdef0123456",
        "89abcdef01234567", "9abcdef012345678",
        "abcdef0123456789", "bcdef0123456789a",
        "cdef0123456789ab", "def0123456789abc",
        "ef0123456789abcd", "f0123456789abcde",
        "fedcba9876543210",
        "048c159d26ae37bf",
        "7e5c3a18f6d4b290",
    };

    /* Same arguments as the selftest, and for the same reason: on a mixed
     * rig the device that has never run these kernels is rarely number 0. An
     * unknown dash argument is refused rather than taken for a chain order. */
    for ( int a = 1; a < argc; a++ )
    {
        if ( strcmp( argv[a], "--list" ) == 0 )
            listing = 1;
        else if ( strcmp( argv[a], "--device" ) == 0 && a + 1 < argc )
            device = atoi( argv[++a] );
        else if ( argv[a][0] == '-' )
        {
            printf( "usage: avian-ocl-batchtest [--list] [--device N] "
                    "[<order>]\n" );
            return 1;
        }
        else
            order = argv[a];
    }

    h = OCL_TEST_OPEN( OCL_TEST_LIB );
    if ( h == NULL )
    {
#ifdef _WIN32
        printf( "cannot load %s: Windows error %lu\n", OCL_TEST_LIB,
                (unsigned long) GetLastError() );
#else
        printf( "cannot load %s: %s\n", OCL_TEST_LIB, dlerror() );
#endif
        return 1;
    }
    b.abi    = OCL_TEST_SYM( h, "avian_cuda_abi_version" );
    b.count  = OCL_TEST_SYM( h, "avian_cuda_device_count" );
    b.name   = OCL_TEST_SYM( h, "avian_cuda_device_name" );
    b.setlog = OCL_TEST_SYM( h, "avian_cuda_set_logger" );
    b.scan   = OCL_TEST_SYM( h, "avian_cuda_scan_batch" );
    if ( !b.abi || !b.count || !b.name || !b.setlog || !b.scan )
    {
        printf( "an entry point is missing\n" );
        return 1;
    }
    if ( b.abi() != AVIAN_CUDA_ABI_VERSION )
    {
        printf( "abi %d, expected %d\n", b.abi(), AVIAN_CUDA_ABI_VERSION );
        return 1;
    }

    b.setlog( logger );
    devices = b.count();
    if ( devices < 1 )
    {
        /* Not a failure: a machine with no OpenCL GPU cannot run this, and
         * saying so beats exiting non-zero and looking like a broken chain. */
        printf( "no OpenCL device, so nothing was scanned\n" );
        return 0;
    }
    if ( listing )
    {
        for ( int d = 0; d < devices; d++ )
            printf( "  device %d: %s\n", d, b.name( d ) );
        return 0;
    }
    if ( device < 0 || device >= devices )
    {
        printf( "no device %d here; this machine has %d:\n", device, devices );
        for ( int d = 0; d < devices; d++ )
            printf( "  device %d: %s\n", d, b.name( d ) );
        return 1;
    }
    printf( "device %d: %s\n", device, b.name( device ) );

    if ( order != NULL )
        return run_order( &b, order, device );

    for ( size_t i = 0; i < sizeof orders / sizeof orders[0]; i++ )
        failed += run_order( &b, orders[i], device );

    printf( "%zu orders, %d with a problem\n",
            sizeof orders / sizeof orders[0], failed );
    return failed != 0;
}

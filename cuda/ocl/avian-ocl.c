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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define OCL_MKDIR( p )  _mkdir( p )
#else
#include <sys/stat.h>
#include <sys/types.h>
#define OCL_MKDIR( p )  mkdir( p, 0700 )
#endif

#include <CL/cl.h>

#include "cuda/avian-cuda-api.h"
#include "cuda/ocl/avian-ocl-kernels.h"

/* Enough for any rig. The loader has its own limit and will say so first. */
#define AVIAN_OCL_MAX_DEVICES 32

/* cl_amd_device_attribute_query. Declared here rather than included from
 * CL/cl_ext.h, which an SDK need not ship and which a CUDA toolkit's partial
 * CL headers do not. An implementation without the extension simply fails
 * the query and the fallback below applies. */
#ifndef CL_DEVICE_BOARD_NAME_AMD
#define CL_DEVICE_BOARD_NAME_AMD 0x4038
#endif

/* One batch's nonces. Deliberately modest: it was chosen before this backend
 * had run on an AMD card, preferring a first version that is slow and
 * finishes over a fast one that trips a display driver's watchdog.
 *
 * Now measured on one: an RX 6600 mining through this backend sits at 99%
 * load with this default, so it is long enough to hide the host's turnaround
 * on that card and there is nothing to recover by raising it. Do not read a
 * batchtest run as evidence either way. That reads 33% load, which is the
 * test's own shape rather than the backend's: one batch inside a one-second
 * process, with the card idle on both sides of it.
 *
 * A power of two so the rounding below leaves it alone. */
#define AVIAN_OCL_THROUGHPUT ( 1u << 20 )

/* Below this a batch stops being long enough to hide the host's turnaround,
 * which is the thing being tuned. */
#define AVIAN_OCL_MIN_THROUGHPUT ( 1u << 16 )

/* The hash buffer is 64 bytes a nonce, so this ceiling is a gigabyte of it.
 * Cards this backend is meant for have that; the allocation is checked anyway
 * and says plainly when they do not. */
#define AVIAN_OCL_MAX_THROUGHPUT ( 1u << 24 )

/* Passed to clBuildProgram, and recorded in the cache key because a change
 * here changes the compiled output. Empty today: these kernels are upstream's
 * and no flag has been shown to help one yet. */
#define AVIAN_OCL_BUILD_OPTIONS ""

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
    char             vendor[16];     /* "nvidia", "amd", "intel" or "" */

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

/* Which vendor's silicon this is, as ABI 3 spells it.
 *
 * Matched on CL_DEVICE_VENDOR rather than the device name, because the name
 * is whatever the driver feels like: ROCm calls an MI325X
 * "gfx942:sramecc+:xnack-" while HIP calls the same card "AMD Instinct
 * MI325X". The vendor string is the stable one. Substrings and case-folded,
 * since it is a free-text field: "Advanced Micro Devices, Inc.", "NVIDIA
 * Corporation", "Intel(R) Corporation".
 *
 * An unrecognised vendor comes back empty rather than guessed, and the
 * loader treats empty as "cannot be matched to another backend's device",
 * which keeps the card usable and merely gives up on deduplicating it. */
static void ocl_vendor_of( cl_device_id id, char *out, size_t n )
{
    char v[256] = { 0 };

    out[0] = '\0';
    if ( clGetDeviceInfo( id, CL_DEVICE_VENDOR, sizeof v - 1, v, NULL )
         != CL_SUCCESS )
        return;
    for ( char *p = v; *p != '\0'; p++ )
        if ( *p >= 'A' && *p <= 'Z' )
            *p = (char) ( *p - 'A' + 'a' );

    if ( strstr( v, "nvidia" ) != NULL )
        snprintf( out, n, "nvidia" );
    else if ( strstr( v, "advanced micro" ) != NULL || strstr( v, "amd" ) != NULL
              || strstr( v, "ati " ) != NULL )
        snprintf( out, n, "amd" );
    else if ( strstr( v, "intel" ) != NULL )
        snprintf( out, n, "intel" );
}

/* The card's name, preferring the board over the architecture.
 *
 * CL_DEVICE_NAME is whatever the implementation chooses to call the device,
 * and the two AMD stacks disagree: Adrenalin on Windows answers "AMD Radeon
 * RX 6900 XT" and ROCm answers "gfx1032", which names a whole family of
 * parts and tells the owner of an RX 6600 nothing about which card in their
 * rig this is. CL_DEVICE_BOARD_NAME_AMD is the marketing name and is what
 * ROCm has instead.
 *
 * Both are kept when they differ, as "AMD Radeon RX 6600 [gfx1032]", because
 * the architecture is what a kernel build problem is reported against and
 * losing it would cost more than the tidier line is worth. */
static void ocl_name_of( cl_device_id id, char *out, size_t n )
{
    char board[160] = { 0 };
    char arch[160]  = { 0 };

    out[0] = '\0';
    clGetDeviceInfo( id, CL_DEVICE_NAME, sizeof arch, arch, NULL );

    if ( clGetDeviceInfo( id, CL_DEVICE_BOARD_NAME_AMD, sizeof board, board,
                          NULL ) != CL_SUCCESS )
        board[0] = '\0';

    if ( board[0] == '\0' || strcmp( board, arch ) == 0 )
        snprintf( out, n, "%s", arch );
    else if ( arch[0] == '\0' )
        snprintf( out, n, "%s", board );
    else
        snprintf( out, n, "%s [%s]", board, arch );
}

/* The batch size, which `AVIAN_OCL_BATCH` overrides in nonces.
 *
 * An environment variable rather than an option because the right value is a
 * property of the card and is found by sweeping it, which wants no rebuild
 * and no change to the ABI between the miner and its backends. Whatever is
 * asked for is rounded down to a power of two, because the global size has to
 * stay a multiple of the work group, and clamped at both ends.
 *
 * Read once: every device gets the same batch, which is wrong in principle
 * for a mixed rig and has not yet been worth the complication of a per-device
 * form of it. */
static uint32_t settle_throughput( void )
{
    static uint32_t settled = 0;
    const char     *want;
    unsigned long   asked;
    uint32_t        n;

    if ( settled != 0 )
        return settled;

    settled = AVIAN_OCL_THROUGHPUT;
    want    = getenv( "AVIAN_OCL_BATCH" );
    if ( want == NULL )
        return settled;

    asked = strtoul( want, NULL, 0 );
    if ( asked < AVIAN_OCL_MIN_THROUGHPUT )
    {
        ocl_log( OCL_LOG_ERR, "OpenCL: AVIAN_OCL_BATCH=%s is below %u nonces, "
                              "so %u a batch stands", want,
                 (unsigned) AVIAN_OCL_MIN_THROUGHPUT, settled );
        return settled;
    }

    for ( n = AVIAN_OCL_MIN_THROUGHPUT;
          n < AVIAN_OCL_MAX_THROUGHPUT && (unsigned long) ( n << 1 ) <= asked;
          n <<= 1 )
        ;
    settled = n;
    ocl_log( OCL_LOG_INFO, "OpenCL: AVIAN_OCL_BATCH=%s, so %u nonces a batch",
             want, settled );
    return settled;
}

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
            dev->throughput = settle_throughput();
            ocl_name_of( ids[d], dev->name, sizeof dev->name );
            ocl_vendor_of( ids[d], dev->vendor, sizeof dev->vendor );
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

/* The compiled program, kept between runs.
 *
 * OpenCL compiles at run time, which is what lets this backend work on a card
 * that did not exist when it was built, and the bill for that is a compile
 * every start: measured at about five seconds for the thirty-three sources on
 * one device, and a rig pays it per card before any of them mine.
 *
 * So the built program is saved and reloaded. The saving is best effort
 * throughout: every failure here falls through to compiling from source,
 * because a cache that can break mining is worse than no cache.
 *
 * What makes a cached binary valid is the question that matters, since a
 * stale one that still loads would hash wrongly. The file therefore carries
 * the full identity it was built for, in full, and that is compared byte for
 * byte rather than by hash: the platform and device names, the driver and
 * OpenCL versions, the build options, the ABI version, and a hash of all the
 * kernel source. Only a driver that changed its compiler without changing
 * any of its version strings could get past that, and nothing can detect
 * that. The backstop is the one already in the ABI: the miner re-hashes every
 * candidate, so a wrong program costs throughput and logs, never a bad share.
 */

#define AVIAN_OCL_CACHE_MAGIC "AVNOCL1"   /* bump when this layout changes */

/* Paths use forward slashes on both platforms. Win32 accepts them throughout,
 * including _mkdir, and it keeps a backslash out of every string here.
 *
 * A failed mkdir is ignored rather than inspected. What errno holds after
 * mkdir of a directory that already exists is not worth relying on across
 * the two platforms, and "C:" as a first component fails on Windows for
 * reasons that are not EEXIST. The only test that matters is whether the file
 * can then be opened, and both callers already treat that failing as an
 * ordinary miss.
 */
static void ocl_mkdir_p( char *path )
{
    for ( char *p = path + 1; ; p++ )
    {
        int end = ( *p == '\0' );

        if ( *p == '/' || end )
        {
            *p = '\0';
            (void) OCL_MKDIR( path );
            if ( end )
                return;
            *p = '/';
        }
    }
}

/* Where to keep it, or 0 if nowhere sensible.
 *
 * AVIAN_OCL_CACHE names a directory, and set to nothing or to "none" turns
 * the cache off, which is how to compare a cold compile against a warm one.
 */
static int cache_dir( char *out, size_t n )
{
    const char *base = getenv( "AVIAN_OCL_CACHE" );

    if ( base != NULL )
    {
        if ( base[0] == '\0' || strcmp( base, "none" ) == 0 )
            return 0;
        if ( (size_t) snprintf( out, n, "%s", base ) >= n )
            return 0;
        ocl_mkdir_p( out );
        return 1;
    }

#ifdef _WIN32
    base = getenv( "LOCALAPPDATA" );
    if ( base == NULL || base[0] == '\0' )
        return 0;
    if ( (size_t) snprintf( out, n, "%s/avian-miner/ocl", base ) >= n )
        return 0;
#else
    base = getenv( "XDG_CACHE_HOME" );
    if ( base != NULL && base[0] != '\0' )
    {
        if ( (size_t) snprintf( out, n, "%s/avian-miner/ocl", base ) >= n )
            return 0;
    }
    else
    {
        base = getenv( "HOME" );
        if ( base == NULL || base[0] == '\0' )
            return 0;
        if ( (size_t) snprintf( out, n, "%s/.cache/avian-miner/ocl", base ) >= n )
            return 0;
    }
#endif
    ocl_mkdir_p( out );
    return 1;
}

/* FNV-1a over all the kernel source, so a changed kernel invalidates the
 * cache without anyone having to remember to bump a version. Not a
 * cryptographic hash and does not need to be: it is detecting an edit, not
 * resisting one, and the identity string next to it is compared in full. */
static uint64_t source_hash( void )
{
    static uint64_t cached = 0;
    uint64_t        h;

    if ( cached != 0 )
        return cached;

    h = 1469598103934665603ULL;
    for ( size_t i = 0; i < AVIAN_OCL_KERNEL_COUNT; i++ )
    {
        const char *p = avian_ocl_kernels[i].entry;

        for ( ; *p != '\0'; p++ )
            h = ( h ^ (unsigned char) *p ) * 1099511628211ULL;
        for ( p = avian_ocl_kernels[i].source; *p != '\0'; p++ )
            h = ( h ^ (unsigned char) *p ) * 1099511628211ULL;
    }
    cached = h ? h : 1;
    return cached;
}

/* Everything about this device and this build that could change the compiled
 * output, NUL separated so two fields cannot run together and look like one.
 * Returns the length written, or 0 if it did not fit. */
static size_t device_identity( struct ocl_device *dev, char *out, size_t n )
{
    static const cl_platform_info pinfo[] = { CL_PLATFORM_NAME, CL_PLATFORM_VERSION };
    static const cl_device_info   dinfo[] = { CL_DEVICE_NAME, CL_DRIVER_VERSION,
                                              CL_DEVICE_VERSION, CL_DEVICE_OPENCL_C_VERSION };
    size_t used = 0;

    for ( size_t i = 0; i < sizeof pinfo / sizeof pinfo[0]; i++ )
    {
        size_t got = 0;

        if ( clGetPlatformInfo( dev->platform, pinfo[i], n - used - 1,
                                out + used, &got ) != CL_SUCCESS || got == 0 )
            return 0;
        used += got;                    /* got includes the NUL */
        if ( used >= n )
            return 0;
    }
    for ( size_t i = 0; i < sizeof dinfo / sizeof dinfo[0]; i++ )
    {
        size_t got = 0;

        if ( clGetDeviceInfo( dev->id, dinfo[i], n - used - 1,
                              out + used, &got ) != CL_SUCCESS || got == 0 )
            return 0;
        used += got;
        if ( used >= n )
            return 0;
    }

    {
        int wrote = snprintf( out + used, n - used, "abi=%d;opts=%s",
                              AVIAN_CUDA_ABI_VERSION, AVIAN_OCL_BUILD_OPTIONS );

        if ( wrote < 0 || (size_t) wrote >= n - used )
            return 0;
        used += (size_t) wrote + 1;
    }
    return used;
}

static int cache_path( struct ocl_device *dev, const char *id, size_t idlen,
                       char *out, size_t n )
{
    char     dir[768];
    uint64_t h = 1469598103934665603ULL;

    if ( !cache_dir( dir, sizeof dir ) )
        return 0;

    /* The file name is a hash of the identity, so it is short and contains
     * nothing a filesystem minds, and the identity itself is compared in full
     * inside the file.
     *
     * Only the device and the build options go into the name, never the
     * source hash, so a device has exactly one file and a changed kernel
     * overwrites it. With the source hash in the name instead, every kernel
     * edit or upgrade would leave another two megabytes behind for ever and
     * something would have to sweep up. The source hash is stored in the file
     * and checked there. */
    for ( size_t i = 0; i < idlen; i++ )
        h = ( h ^ (unsigned char) id[i] ) * 1099511628211ULL;

    (void) dev;
    return (size_t) snprintf( out, n, "%s/%016llx.bin", dir,
                              (unsigned long long) h ) < n;
}

/* A program from the cache, or NULL. Says nothing on an ordinary miss. */
static cl_program cache_load( struct ocl_device *dev, const char *id,
                              size_t idlen )
{
    char        path[1024];
    FILE       *f;
    char        magic[sizeof AVIAN_OCL_CACHE_MAGIC];
    uint32_t    stored_idlen = 0, binlen = 0;
    uint64_t    stored_src = 0;
    char       *stored_id = NULL;
    unsigned char *bin = NULL;
    size_t      binsz;
    cl_int      err = CL_SUCCESS, status = CL_SUCCESS;
    cl_program  prog = NULL;

    if ( !cache_path( dev, id, idlen, path, sizeof path ) )
        return NULL;
    f = fopen( path, "rb" );
    if ( f == NULL )
        return NULL;

    if ( fread( magic, 1, sizeof magic, f ) != sizeof magic
         || memcmp( magic, AVIAN_OCL_CACHE_MAGIC, sizeof magic ) != 0
         || fread( &stored_idlen, sizeof stored_idlen, 1, f ) != 1
         || stored_idlen != idlen )
        goto done;

    stored_id = malloc( stored_idlen );
    if ( stored_id == NULL
         || fread( stored_id, 1, stored_idlen, f ) != stored_idlen
         || memcmp( stored_id, id, idlen ) != 0
         || fread( &stored_src, sizeof stored_src, 1, f ) != 1
         || stored_src != source_hash()
         || fread( &binlen, sizeof binlen, 1, f ) != 1
         || binlen == 0 || binlen > ( 512u << 20 ) )
        goto done;

    bin = malloc( binlen );
    if ( bin == NULL || fread( bin, 1, binlen, f ) != binlen )
        goto done;

    binsz = binlen;
    prog = clCreateProgramWithBinary( dev->cx, 1, &dev->id, &binsz,
                                      (const unsigned char **) &bin,
                                      &status, &err );
    if ( err != CL_SUCCESS || status != CL_SUCCESS )
    {
        if ( prog != NULL )
            clReleaseProgram( prog );
        prog = NULL;
        goto done;
    }
    /* Still required after loading a binary, and fast: this is the step that
     * rejects a binary the driver no longer accepts. */
    if ( clBuildProgram( prog, 1, &dev->id, AVIAN_OCL_BUILD_OPTIONS,
                         NULL, NULL ) != CL_SUCCESS )
    {
        clReleaseProgram( prog );
        prog = NULL;
    }

done:
    free( stored_id );
    free( bin );
    fclose( f );
    return prog;
}

/* Best effort, and quiet about it: a rig with a read-only home still mines,
 * it just compiles every start. */
static void cache_store( struct ocl_device *dev, const char *id, size_t idlen )
{
    char            path[1024], tmp[1100];
    FILE           *f;
    size_t          binsz = 0;
    unsigned char  *bin;
    uint64_t        src = source_hash();
    uint32_t        idlen32 = (uint32_t) idlen, binlen32;
    int             ok;

    if ( !cache_path( dev, id, idlen, path, sizeof path ) )
        return;
    if ( clGetProgramInfo( dev->prog, CL_PROGRAM_BINARY_SIZES,
                           sizeof binsz, &binsz, NULL ) != CL_SUCCESS
         || binsz == 0 )
        return;                 /* some implementations have no binary to give */

    bin = malloc( binsz );
    if ( bin == NULL )
        return;
    if ( clGetProgramInfo( dev->prog, CL_PROGRAM_BINARIES, sizeof bin, &bin,
                           NULL ) != CL_SUCCESS )
    {
        free( bin );
        return;
    }

    /* Through a temporary and renamed, so two miners starting together cannot
     * leave a half written file for the next run to load. */
    if ( (size_t) snprintf( tmp, sizeof tmp, "%s.tmp", path ) >= sizeof tmp )
    {
        free( bin );
        return;
    }
    f = fopen( tmp, "wb" );
    if ( f == NULL )
    {
        free( bin );
        return;
    }
    binlen32 = (uint32_t) binsz;
    ok = fwrite( AVIAN_OCL_CACHE_MAGIC, 1, sizeof AVIAN_OCL_CACHE_MAGIC, f )
             == sizeof AVIAN_OCL_CACHE_MAGIC
         && fwrite( &idlen32, sizeof idlen32, 1, f ) == 1
         && fwrite( id, 1, idlen, f ) == idlen
         && fwrite( &src, sizeof src, 1, f ) == 1
         && fwrite( &binlen32, sizeof binlen32, 1, f ) == 1
         && fwrite( bin, 1, binsz, f ) == binsz;
    if ( fclose( f ) != 0 )
        ok = 0;
    free( bin );

    if ( !ok )
    {
        remove( tmp );
        return;
    }
    /* rename over an existing file fails on Windows, so clear the way. */
    remove( path );
    if ( rename( tmp, path ) != 0 )
        remove( tmp );
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
    char        id[2048];
    size_t      idlen;
    cl_int      err;

    idlen = device_identity( dev, id, sizeof id );
    if ( idlen != 0 )
    {
        dev->prog = cache_load( dev, id, idlen );
        if ( dev->prog != NULL )
        {
            ocl_log( OCL_LOG_INFO, "OpenCL: %s: kernels from the cache",
                     dev->name );
            return 1;
        }
    }

    /* Said here rather than before the call, so it is only said when there is
     * really a compile to wait for. */
    ocl_log( OCL_LOG_INFO, "OpenCL: %s: compiling the kernels, which takes a "
                           "moment on first use", dev->name );

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
    if ( clBuildProgram( dev->prog, 1, &dev->id, AVIAN_OCL_BUILD_OPTIONS,
                         NULL, NULL ) != CL_SUCCESS )
    {
        ocl_log( OCL_LOG_ERR, "OpenCL: %s: the kernels would not build", dev->name );
        log_build_failure( dev );
        return 0;
    }
    if ( idlen != 0 )
        cache_store( dev, id, idlen );
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

AVIAN_CUDA_API const char *avian_cuda_device_vendor( int dev_id )
{
    enumerate();
    if ( dev_id < 0 || dev_id >= s_devices )
        return "";
    return s_device[dev_id].vendor;
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
    uint32_t  hdr[20];
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
     * nonces from a batch, and the miner is told what was actually tried.
     *
     * Never to nothing, though. A caller asking for fewer nonces than one
     * group rounds to zero, and a batch that tries nothing cannot advance the
     * caller's nonce, so its scan loop spins for ever on empty launches. The
     * host's first call does exactly that: the miner sizes its window from a
     * nominal 20 H/s until a scan has been timed, which is a hundred nonces,
     * and --gpu --benchmark then hung before printing a single rate. The CUDA
     * backend carries the same floor for the same reason. One group over the
     * window is a few hundred nonces past where the caller asked to stop,
     * which is why the count comes back rather than being assumed. */
    global -= global % dev->local;
    if ( global == 0 )
        global = dev->local;

    target = ( (cl_ulong) job->target[7] << 32 ) | (cl_ulong) job->target[6];

    /* The header goes up once a batch; only the nonce differs per work item,
     * and the kernels derive that from their global id.
     *
     * Byte-swapped on the way, which is the ABI: job.data is "the header in
     * byte-swapped words" and the CUDA backend has always built the same
     * copy before handing it to a kernel. This backend used to upload
     * job.data as it stood and hash that, so it hashed a different eighty
     * bytes from the ones the miner re-hashes and submits. Those were real
     * hashes found at the right rate, of the wrong message: every candidate
     * cleared the device's target and every one of them failed
     * fulltest, so the backend could never submit a share. The nonce word
     * is not swapped here because the first-round kernel overwrites bytes
     * 76 to 79 anyway; it writes the nonce big-endian, to match. */
    for ( int i = 0; i < 20; i++ )
    {
        unsigned char *p = (unsigned char *) &hdr[i];
        uint32_t       v = job->data[i];

        p[0] = (unsigned char) ( v >> 24 );
        p[1] = (unsigned char) ( v >> 16 );
        p[2] = (unsigned char) ( v >> 8 );
        p[3] = (unsigned char) v;
    }
    err = clEnqueueWriteBuffer( dev->q, dev->m_header, CL_FALSE, 0, 80,
                                hdr, 0, NULL, NULL );
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

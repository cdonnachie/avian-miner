/* A backend that implements the ABI and owns no hardware.
 *
 * Built as libavian_amd.so (or avian_amd.dll) it makes the loader's dispatch
 * testable on a machine with one vendor, or none: the thing that cannot
 * otherwise be checked without owning an Nvidia and an AMD card at once is
 * whether devices from two backends are numbered as one list and whether a
 * batch reaches the right one with the right local index.
 *
 * It answers the question by reporting what it was asked for. Every call
 * prints its arguments through the miner's own logger, so the test asserts
 * on the miner's output rather than on anything this file decides.
 *
 * Deliberately not part of any build. avian/gpu-dispatch-test.sh compiles it
 * into a temporary directory beside copies of the miner, so a stub can never
 * be left where a real build would find it. See doc/building.md.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cuda/avian-cuda-api.h"

#define STUB_LOG_INFO 6
#define STUB_MAX_DEVICES 8

/* How many devices to claim. Set by the test, because the interesting cases
 * are one and several: with two, a global index of 1 has to arrive here as a
 * local 0, which is the whole mapping under test. */
static int device_total( void )
{
    const char *n = getenv( "AVIAN_GPU_STUB_DEVICES" );
    int count = n ? atoi( n ) : 2;

    if ( count < 0 )
        count = 0;
    return count > STUB_MAX_DEVICES ? STUB_MAX_DEVICES : count;
}

/* Reported instead of the real version when the test asks, which is how the
 * loader's refusal of a mismatched backend gets exercised. Not a -D on the
 * compile: the header defines AVIAN_CUDA_ABI_VERSION unconditionally and
 * would win. */
static int abi_claimed( void )
{
    const char *v = getenv( "AVIAN_GPU_STUB_ABI" );

    return v ? atoi( v ) : AVIAN_CUDA_ABI_VERSION;
}

static avian_cuda_log_fn g_log = NULL;

static void say( const char *fmt, ... )
{
    char buf[512];
    va_list ap;

    if ( g_log == NULL )
        return;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof(buf), fmt, ap );
    va_end( ap );
    g_log( STUB_LOG_INFO, buf );
}

AVIAN_CUDA_API int avian_cuda_abi_version( void )
{
    return abi_claimed();
}

AVIAN_CUDA_API void avian_cuda_set_logger( avian_cuda_log_fn fn )
{
    g_log = fn;
}

AVIAN_CUDA_API int avian_cuda_device_count( void )
{
    return device_total();
}

AVIAN_CUDA_API const char *avian_cuda_device_name( int dev_id )
{
    static char name[STUB_MAX_DEVICES][64];

    if ( dev_id < 0 || dev_id >= device_total() )
        return "";
    /* The index is in the name, so the test can tell from the miner's own
     * enumeration which local device each global one resolved to. */
    snprintf( name[dev_id], sizeof(name[0]), "STUB AMD device %d", dev_id );
    return name[dev_id];
}

AVIAN_CUDA_API void avian_cuda_set_diagnostics( avian_cuda_hash_n_fn hash_n,
                                                avian_cuda_prehash_fn prehash )
{
    say( "STUB: diagnostics wired (hash_n %s, prehash %s)",
         hash_n ? "set" : "null", prehash ? "set" : "null" );
}

/* Reports no candidates, which is a legal answer and keeps the miner out of
 * its verification path: this file has no hashes to be right or wrong about.
 * The throughput it claims is what moves the nonce on, so the miner's own
 * loop terminates rather than spinning. */
AVIAN_CUDA_API int avian_cuda_scan_batch( const avian_cuda_job *job,
                                          uint32_t *out_nonces, int max_out,
                                          uint32_t *throughput_used )
{
    (void) out_nonces;
    (void) max_out;

    say( "STUB: batch thr_id=%d device=%d start_nonce=%08x order=%s",
         job->thr_id, job->device, job->start_nonce, job->order );
    if ( throughput_used )
        *throughput_used = 0x10000;
    return 0;
}

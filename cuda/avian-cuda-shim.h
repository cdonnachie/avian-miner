#ifndef AVIAN_CUDA_SHIM_H
#define AVIAN_CUDA_SHIM_H 1

/* What the vendored kernels used to get from ccminer's miner.h.
 *
 * They need very little: applog with its log levels, _ALIGN and opt_debug,
 * which this miner's own miner.h already provides, plus MAX_GPUS and
 * device_map, which it does not.
 *
 * miner.h is included rather than the handful of names being redeclared,
 * because LOG_ERR and LOG_WARNING come from <syslog.h> when the build has it
 * and from a local enum when it does not, which gives them different values.
 * Two declarations of that could drift apart silently and misroute log
 * output, so there is deliberately only one.
 *
 * Do NOT include cuda_helper.h here, however tempting, even though it also
 * declares MAX_GPUS and device_map. cuda_helper.h and cuda_helper_alexis.h
 * are interchangeable variants of one header and share the include guard
 * CUDA_HELPER_H, so whichever arrives first silently suppresses the other.
 * Ten of the vendored kernels want the first and seventeen want the second,
 * and none want both. Pulling one in from here disabled the other variant in
 * seventeen files, which surfaced as symbols like SWAPDWORDS2 and bfe simply
 * going missing rather than as any complaint about the header.
 */

/* miner.h has no extern "C" guards, and these are C++ translation units, so
 * including it plainly gives applog and fulltest C++ mangled names while the
 * miner defines C ones, and the link fails on symbols that look present.
 *
 * The system headers miner.h pulls in are included first, outside the linkage
 * block, so only miner.h's own declarations land inside it. Their include
 * guards make the copies inside miner.h no-ops. Several of these, curl and
 * jansson among them, carry their own extern "C" and would be harmless
 * nested, but mm_malloc.h and getopt.h are less predictable and this costs
 * nothing.
 */
#ifdef AVIAN_CUDA_BUILDING_DLL

/* The DLL build. The kernels need a logger, the log levels, an alignment
 * macro and the byte-order helpers, and nothing else: the split moved
 * struct work and every miner-side call to the other side of the ABI, so
 * miner.h and the POSIX headers it needs no longer come into it. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* The vendored kernels' CUDA_SAFE_CALL expands to fprintf(stderr, ...) and
 * exit(), so stdio and stdlib come with them. */
#include <stdio.h>
#include <stdlib.h>

#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#define LOG_BLUE    0x10

#ifndef _ALIGN
#define _ALIGN(x) __declspec(align(x))
#endif

/* __thread is a GCC extension; MSVC spells it __declspec(thread). Nine of
 * the vendored kernels use it for per-device init flags, and defining it
 * here keeps those files byte-identical to upstream, which is the whole
 * reason they are easy to re-vendor. */
#ifdef _MSC_VER
#  ifndef __thread
#    define __thread __declspec(thread)
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

void applog(int level, const char *fmt, ...);

/* Set from the job on each batch, so the kernels' own debug paths follow
 * the miner's setting without the two sharing a global across the ABI. */
extern int opt_debug;

static __inline void be32enc(void *pp, uint32_t x)
{
    unsigned char *p = (unsigned char *) pp;
    p[3] = (unsigned char) (x & 0xff);
    p[2] = (unsigned char) ((x >> 8) & 0xff);
    p[1] = (unsigned char) ((x >> 16) & 0xff);
    p[0] = (unsigned char) ((x >> 24) & 0xff);
}

static __inline uint32_t be32dec(const void *pp)
{
    const unsigned char *p = (const unsigned char *) pp;
    return ((uint32_t) p[3]) | (((uint32_t) p[2]) << 8)
         | (((uint32_t) p[1]) << 16) | (((uint32_t) p[0]) << 24);
}

#ifdef __cplusplus
}
#endif

#else

#ifdef __cplusplus

#include <stdbool.h>
#include <inttypes.h>
#include "cuda/avian-cuda-win-compat.h"
#include "cuda/avian-cuda-win-compat.h"
#include <pthread.h>
#include <jansson.h>
#include <curl/curl.h>
#include <syslog.h>
#include <getopt.h>

extern "C" {
#include "miner.h"
}

#else

/* x16rt-cuda-gate.c is C and reaches this header through
 * avian-cuda-device.h, where a linkage block would be a syntax error. */
#include "miner.h"

#endif

#endif /* AVIAN_CUDA_BUILDING_DLL */

/* Both helper variants define this identically; guarded so that including
 * either one after this header stays quiet. */
#ifndef MAX_GPUS
#define MAX_GPUS 16
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Logical device index to CUDA device ordinal. Defined in cuda/device.cu. */
extern short device_map[MAX_GPUS];

/* ccminer's "benchmark every algorithm in turn" mode, which this miner does
 * not have. The vendored hash check returns early while such a sweep runs, so
 * this is pinned at -1 and the check always happens. */
extern int bench_algo;

#ifdef __cplusplus
}
#endif

#endif /* AVIAN_CUDA_SHIM_H */

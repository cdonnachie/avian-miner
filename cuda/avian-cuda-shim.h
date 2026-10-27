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
#ifdef __cplusplus

#include <stdbool.h>
#include <inttypes.h>
#include <sys/time.h>
#include <unistd.h>
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

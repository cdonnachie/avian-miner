#!/usr/bin/env python3
"""Give the DLL build a minimal shim instead of the miner's whole header.

avian-cuda-shim.h pre-includes pthread, curl, jansson, syslog and getopt so
that miner.h can be wrapped in extern "C". That is right for the in-process
Linux build and wrong for the DLL, where MSVC has none of those and, more to
the point, the DLL no longer needs miner.h: moving verification and
submission to the miner removed every use of struct work.

So under AVIAN_CUDA_BUILDING_DLL the shim declares the handful of things the
kernels actually use and stops there. Chasing five POSIX headers into MSVC
would have been work in service of an include the split had already made
unnecessary.

Applied to the staged Windows copy only. The Linux tree keeps the real shim,
and both are compiled from the same kernels, so the arrangement is checked by
every Linux build rather than only on Windows.
"""
import io
import sys

W = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/development/cpuminer-avian"
P = W + "/cuda/avian-cuda-shim.h"

MARK = "AVIAN_CUDA_BUILDING_DLL"

MINIMAL = '''#ifdef AVIAN_CUDA_BUILDING_DLL

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
'''


def main():
    s = io.open(P, encoding="utf-8", errors="surrogateescape", newline="").read()
    if MARK in s:
        print("  shim already has a DLL path")
        return 0

    anchor = "#ifdef __cplusplus\n\n#include <stdbool.h>"
    if s.count(anchor) != 1:
        print("  ERROR: shim anchor matched %d times" % s.count(anchor))
        return 1

    s = s.replace(anchor, MINIMAL + "\n" + anchor, 1)

    # Close the new #ifdef where the miner.h branch ends.
    tail = '#include "miner.h"\n\n#endif\n'
    if s.count(tail) != 1:
        print("  ERROR: shim tail matched %d times" % s.count(tail))
        return 1
    s = s.replace(tail, tail + "\n#endif /* AVIAN_CUDA_BUILDING_DLL */\n", 1)

    io.open(P, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("  shim given a DLL path that needs no miner.h")
    return 0


if __name__ == "__main__":
    sys.exit(main())

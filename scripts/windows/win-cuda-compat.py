#!/usr/bin/env python3
"""Make the staged CUDA sources compile under MSVC.

The kernels were written for a POSIX host compiler and reach for unistd.h and
sleep(). MSVC has neither. Applied to the Windows copies only, so the Linux
tree that was just validated by a mining run is untouched: one of today's
lessons is that a Windows-only divergence nothing tests is where defects go
to hide, so the divergence here is kept to the smallest possible thing, a
header guard and a one-line sleep.

Re-runnable, and it asserts rather than silently skipping, because a
portability patch that quietly does nothing produces a build failure three
minutes later with a confusing message.
"""
import io
import os
import re
import sys

# Derived rather than named, so a clone anywhere works. The argument
# still wins, which is how the callers pass the build tree instead.
W = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

COMPAT_H = '''#ifndef AVIAN_CUDA_WIN_COMPAT_H
#define AVIAN_CUDA_WIN_COMPAT_H 1

/* The kernels' host side was written against POSIX. MSVC is the only host
 * compiler nvcc will use on Windows, so the few POSIX calls they make need
 * somewhere to land. Deliberately tiny: the alternative is editing the
 * vendored kernels, and a vendored file that differs per platform is one
 * nobody will diff against upstream again. */

#ifdef _WIN32
/* winsock2.h before windows.h, or the two fight over the socket
 * definitions and the errors are nothing to do with the real problem.
 * It is also where struct timeval comes from. */
#  include <winsock2.h>
#  include <windows.h>

/* POSIX sleep takes seconds; Windows Sleep takes milliseconds. Getting this
 * wrong is a thousand-fold error that looks like a hang. */
static __inline unsigned int avian_win_sleep(unsigned int seconds)
{
    Sleep(seconds * 1000u);
    return 0;
}
#  define sleep(s) avian_win_sleep((unsigned int)(s))

static __inline int avian_win_usleep(unsigned long usec)
{
    Sleep((DWORD)((usec + 999ul) / 1000ul));
    return 0;
}
#  define usleep(u) avian_win_usleep((unsigned long)(u))

/* Only what the kernels' host side actually uses: a wall clock for timing
 * log lines. Not a general gettimeofday, and not pretending to be one. */
static __inline int avian_win_gettimeofday(struct timeval *tv, void *tz)
{
    FILETIME ft;
    ULARGE_INTEGER t;

    (void) tz;
    if (tv == NULL) {
        return -1;
    }
    GetSystemTimeAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    /* FILETIME counts 100ns ticks from 1601; shift to the Unix epoch. */
    t.QuadPart -= 116444736000000000ULL;
    tv->tv_sec = (long) (t.QuadPart / 10000000ULL);
    tv->tv_usec = (long) ((t.QuadPart % 10000000ULL) / 10ULL);
    return 0;
}
#  define gettimeofday(tv, tz) avian_win_gettimeofday((tv), (tz))

#else
#  include <unistd.h>
#  include <sys/time.h>
#endif

#endif /* AVIAN_CUDA_WIN_COMPAT_H */
'''


def main():
    hdr = os.path.join(W, "cuda", "avian-cuda-win-compat.h")
    io.open(hdr, "w", encoding="utf-8", newline="").write(COMPAT_H)
    print("  wrote cuda/avian-cuda-win-compat.h")

    # Every POSIX header the kernels' host side reaches for. The compat
    # header supplies all of them, so a file including two of them ends up
    # including the compat header twice, which its guard handles.
    pat = re.compile(
        r'^([ \t]*)#include[ \t]*<(?:unistd\.h|sys/time\.h)>[ \t]*$', re.M)
    repl = r'\1#include "cuda/avian-cuda-win-compat.h"'

    # Only files MSVC compiles. The miner's own sources and headers are built
    # by MinGW, which has these POSIX headers, and _WIN32 is defined there
    # too: pointing them at this compat header would force the whole miner
    # down the MSVC path and redefine sleep and gettimeofday underneath it.
    # An earlier version of this filter caught algo/x16/x16r-gate.h and would
    # have done exactly that.
    ONLY = {os.path.join("algo", "x16", "x16rt-cuda-batch.cu")}

    patched, already = [], []
    for root, _dirs, files in os.walk(W):
        rel = os.path.relpath(root, W)
        in_cuda = rel == "cuda" or rel.startswith("cuda" + os.sep)
        for f in files:
            relf = os.path.normpath(os.path.join(rel, f))
            if not in_cuda and relf not in ONLY:
                continue
            if not f.endswith((".cu", ".cuh", ".h")):
                continue
            p = os.path.join(root, f)
            s = io.open(p, encoding="utf-8", errors="surrogateescape",
                        newline="").read()
            if "avian-cuda-win-compat.h" in s:
                already.append(os.path.relpath(p, W))
                continue
            s2, n = pat.subn(repl, s)
            if n:
                io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                        newline="").write(s2)
                patched.append("%s (%d)" % (os.path.relpath(p, W), n))

    for x in patched:
        print("  patched %s" % x)
    for x in already:
        print("  already patched %s" % x)
    if not patched and not already:
        print("  ERROR: no file included <unistd.h>; the anchor is wrong")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

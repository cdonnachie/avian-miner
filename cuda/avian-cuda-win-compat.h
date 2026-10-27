#ifndef AVIAN_CUDA_WIN_COMPAT_H
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

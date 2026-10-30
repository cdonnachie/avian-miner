# The package name and version reach pools as the User-Agent and in
# mining.subscribe, via USER_AGENT in miner.h, so both are this
# project's own. They were cpuminer-opt's, which told an operator
# nothing about which fork had connected, and then avian-miner-26.1,
# which reads as a version of this and is upstream's: two builds
# thirteen commits apart both reported it. What this was forked from
# is history rather than a property of the package, so the banner in
# cpu-miner.c names cpuminer-opt 26.1 literally.
# AC_CONFIG_HEADERS below pins cpuminer-config.h, so renaming the
# package does not rename the generated header.
AC_INIT([avian-miner], [1.0.0])

AC_PREREQ([2.59c])
AC_CANONICAL_SYSTEM
AC_CONFIG_SRCDIR([cpu-miner.c])
AC_CONFIG_HEADERS([cpuminer-config.h])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign subdir-objects])

dnl Make sure anyone changing configure.ac/Makefile.am has a clue
AM_MAINTAINER_MODE

dnl Checks for programs
AC_PROG_CC_C99
AC_PROG_GCC_TRADITIONAL
AM_PROG_CC_C_O
AM_PROG_AS
AC_PROG_RANLIB
AC_PROG_CXX

dnl Checks for header files
AC_HEADER_STDC
AC_CHECK_HEADERS([sys/endian.h sys/param.h syslog.h])
# sys/sysctl.h requires sys/types.h on FreeBSD
# sys/sysctl.h requires sys/param.h on OpenBSD
AC_CHECK_HEADERS([sys/sysctl.h], [], [],
[#include <sys/types.h>
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
])

AC_CHECK_DECLS([be32dec, le32dec, be32enc, le32enc, le16dec, le16enc], [], [],
[AC_INCLUDES_DEFAULT
#ifdef HAVE_SYS_ENDIAN_H
#include <sys/endian.h>
#endif
])

AC_FUNC_ALLOCA
AC_CHECK_FUNCS([getopt_long])

case $target in
  x86_64-*-*|amd64-*-*)
    have_x86_64=true
    ;;
  aarch64*-*-*|arm64*-*-*)
    have_arm64=true
    ;;
  powerpc*-*-*)
    have_ppc=true
    ;;
esac

PTHREAD_FLAGS="-pthread"
WS2_LIBS=""

case $target in
  *-*-mingw*)
    have_win32=true
    PTHREAD_FLAGS=""
    WS2_LIBS="-lws2_32"
    ;;
  *-apple-*)
    have_apple=true
    ;;
esac

AC_ARG_ENABLE([assembly],
  AS_HELP_STRING([--disable-assembly], [disable assembly-language routines]))
if test x$enable_assembly != xno; then
  AC_DEFINE([USE_ASM], [1], [Define to 1 if assembly routines are wanted.])
fi

# jansson test fails on Linux/Mingw, handled in Makefile.am.
AC_CHECK_LIB(jansson, json_loads, request_jansson=false, request_jansson=true)

AC_CHECK_LIB([pthread], [pthread_create], PTHREAD_LIBS="-lpthread",
  AC_CHECK_LIB([pthreadGC2], [pthread_create], PTHREAD_LIBS="-lpthreadGC2",
    AC_CHECK_LIB([pthreadGC1], [pthread_create], PTHREAD_LIBS="-lpthreadGC1",
      AC_CHECK_LIB([pthreadGC], [pthread_create], PTHREAD_LIBS="-lpthreadGC"
))))

AC_MSG_CHECKING(whether __uint128_t is supported)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([static __uint128_t i = 100;])],
      AC_DEFINE(USE_INT128, 1, [Define if __uint128_t is available])
      AC_MSG_RESULT(yes)
    ,
      AC_MSG_RESULT(no)
)

# allow if <var> in Makefile.am
AM_CONDITIONAL([WANT_JANSSON], [test x$request_jansson = xtrue])
AM_CONDITIONAL([HAVE_WINDOWS], [test x$have_win32 = xtrue])
AM_CONDITIONAL([USE_ASM], [test x$enable_assembly != xno])
AM_CONDITIONAL([ARCH_x86_64], [test x$have_x86_64 = xtrue])
AM_CONDITIONAL([ARCH_ARM64], [test x$have_arm64 = xtrue])
AM_CONDITIONAL([MINGW], [test "x$OS" = "xWindows_NT"])
AM_CONDITIONAL([HAVE_APPLE], [test x$have_apple = xtrue])

# The avian-sv2 workspace, where libavian-sv2 has been built. Without it the
# Stratum v2 backend is left out and the miner builds and runs exactly as
# upstream, Stratum v1 and all.
AC_ARG_WITH([avian-sv2],
   [  --with-avian-sv2=PATH    root of the avian-sv2 workspace, built with
                           cargo build --release -p avian-sv2])

if test -n "$with_avian_sv2" ; then
   AVIAN_SV2_CFLAGS="-I$with_avian_sv2/crates/avian-sv2/include"
   if test -f "$with_avian_sv2/target/release/libavian_sv2.a" ; then
      AVIAN_SV2_LIBS="$with_avian_sv2/target/release/libavian_sv2.a"
   elif test -f "$with_avian_sv2/target/debug/libavian_sv2.a" ; then
      AVIAN_SV2_LIBS="$with_avian_sv2/target/debug/libavian_sv2.a"
   else
      AC_MSG_ERROR([no libavian_sv2.a under $with_avian_sv2/target; build it with cargo build --release -p avian-sv2])
   fi
   AC_DEFINE([HAVE_AVIAN_SV2], [1], [Define to 1 to build the Stratum v2 backend.])
   have_avian_sv2=true
fi
AM_CONDITIONAL([HAVE_AVIAN_SV2], [test x$have_avian_sv2 = xtrue])
AC_SUBST(AVIAN_SV2_CFLAGS)
AC_SUBST(AVIAN_SV2_LIBS)

dnl The CUDA GPU backend. Off unless asked for, so a build without it is
dnl unchanged.
AC_ARG_WITH([cuda],
   [  --with-cuda@<:@=PATH@:>@  build the CUDA X16RT backend; PATH is the toolkit prefix @<:@default=/usr/local/cuda@:>@])

have_nv=false
if test "x$with_cuda" != "x" && test "x$with_cuda" != "xno" ; then
   if test "x$with_cuda" = "xyes" ; then
      cuda_prefix=/usr/local/cuda
   else
      cuda_prefix="$with_cuda"
   fi
   dnl The prefix is checked directly rather than handed to AC_PATH_PROG as a
   dnl search path, which did not find an nvcc that was plainly there.
   AC_MSG_CHECKING([for nvcc])
   if test -x "$cuda_prefix/bin/nvcc" ; then
      NVCC="$cuda_prefix/bin/nvcc"
      AC_MSG_RESULT([$NVCC])
   else
      AC_MSG_RESULT([not under $cuda_prefix/bin])
      AC_PATH_PROG([NVCC], [nvcc], [no])
   fi
   if test "x$NVCC" = "xno" || test "x$NVCC" = "x" ; then
      AC_MSG_ERROR([no nvcc in $cuda_prefix/bin or on PATH; pass --with-cuda=PREFIX])
   fi

   dnl Ask the toolkit which architectures it can emit rather than hard-coding
   dnl them. CUDA 13 dropped Maxwell, Pascal and Volta, and 12.8 was the first
   dnl with Blackwell, so the answer depends on the version installed and a
   dnl fixed list would either fail to build or silently omit cards.
   cuda_avail=`$NVCC --list-gpu-arch 2>/dev/null | sed 's/compute_//' | tr '\n' ' '`
   if test "x$cuda_avail" = "x" ; then
      AC_MSG_ERROR([$NVCC --list-gpu-arch produced nothing; is the toolkit complete?])
   fi

   dnl A release wants every architecture the toolkit can emit, which is nine
   dnl targets over the whole kernel set and takes the better part of an hour.
   dnl While developing, one is enough.
   AC_ARG_WITH([cuda-arch],
      [  --with-cuda-arch=LIST   build only these GPU architectures, e.g. 86 or "86 120" @<:@default=all available@:>@])
   if test "x$with_cuda_arch" != "x" && test "x$with_cuda_arch" != "xno" ; then
      cuda_want="$with_cuda_arch"
   else
      cuda_want="52 61 75 80 86 89 100 120"
   fi

   AC_MSG_CHECKING([GPU architectures])
   NVCC_ARCH=""
   cuda_built=""
   cuda_newest=""
   dnl 52 and 61 are Maxwell and Pascal: a 900 or 10 series card. They cost
   dnl about five minutes of build and seven megabytes, measured, and the
   dnl X16RT tuning tables upstream are full of 1060s and 1080s, so they stay
   dnl while any toolkit still offers them.
   for a in $cuda_want ; do
      for h in $cuda_avail ; do
         if test "x$a" = "x$h" ; then
            NVCC_ARCH="$NVCC_ARCH -gencode=arch=compute_$a,code=sm_$a"
            cuda_built="$cuda_built $a"
            cuda_newest="$a"
         fi
      done
   done
   if test "x$NVCC_ARCH" = "x" ; then
      AC_MSG_ERROR([$NVCC supports none of: $cuda_want])
   fi
   dnl Also emit PTX for the newest architecture this toolkit knows, so a card
   dnl released after the build still runs by driver JIT instead of failing to
   dnl launch at all.
   NVCC_ARCH="$NVCC_ARCH -gencode=arch=compute_$cuda_newest,code=compute_$cuda_newest"
   AC_MSG_RESULT([sm$cuda_built, plus compute_$cuda_newest as PTX])

   CUDA_LDFLAGS="-L$cuda_prefix/lib64 -L$cuda_prefix/lib"
   dnl -lstdc++ because the kernels are C++ while this miner links with CC.
   CUDA_LIBS="-lcudart -lstdc++"
   have_nv=true
fi
# The same kernels, for AMD hardware. This is a smaller change than it looks:
# the miner's own C never names a GPU runtime symbol, it goes through the
# plain-data avian_cuda_* ABI in cuda/avian-cuda-api.h, so the compiler and
# the runtime library are the whole of the difference.
AC_ARG_WITH([hip],
   [  --with-hip@<:@=PATH@:>@   build the HIP X16RT backend for AMD; PATH is the ROCm prefix @<:@default=/opt/rocm@:>@])

have_amd=false
have_gpu=false
if test "x$with_hip" != "x" && test "x$with_hip" != "xno" ; then
   if test "x$with_hip" = "xyes" ; then
      hip_prefix=/opt/rocm
   else
      hip_prefix="$with_hip"
   fi
   AC_MSG_CHECKING([for hipcc])
   if test -x "$hip_prefix/bin/hipcc" ; then
      HIPCC="$hip_prefix/bin/hipcc"
      AC_MSG_RESULT([$HIPCC])
   else
      AC_MSG_RESULT([not under $hip_prefix/bin])
      AC_PATH_PROG([HIPCC], [hipcc], [no])
   fi
   if test "x$HIPCC" = "xno" || test "x$HIPCC" = "x" ; then
      AC_MSG_ERROR([no hipcc in $hip_prefix/bin or on PATH; pass --with-hip=PREFIX])
   fi

   AC_ARG_WITH([hip-arch],
      [  --with-hip-arch=LIST    build these GPU architectures, e.g. gfx942, "gfx942 gfx1100" or gfx942,gfx1100 @<:@default=the GPUs installed here@:>@])
   if test "x$with_hip_arch" != "x" && test "x$with_hip_arch" != "xno" ; then
      dnl Commas as well as spaces: a space-separated list has to be quoted,
      dnl which a configure line assembled from shell variables cannot do, and
      dnl an unquoted one arrives as several unrecognised arguments instead.
      hip_want=`echo "$with_hip_arch" | tr ',' ' '`
   else
      dnl Asked of the machine rather than defaulted to a list, because unlike
      dnl the CUDA side there is no safe guess: a CUDA binary carries PTX and
      dnl a card newer than the build still runs by driver JIT, whereas an AMD
      dnl binary built for the wrong gfx target simply fails to launch.
      AC_MSG_CHECKING([for an installed AMD GPU])
      hip_want=`$hip_prefix/llvm/bin/amdgpu-arch 2>/dev/null | sort -u | tr '
' ' '`
      if test "x$hip_want" = "x" ; then
         hip_want=`amdgpu-arch 2>/dev/null | sort -u | tr '
' ' '`
      fi
      if test "x$hip_want" = "x" ; then
         AC_MSG_RESULT([none found])
         AC_MSG_ERROR([no GPU here to build for; pass --with-hip-arch=gfx942 or similar])
      fi
      AC_MSG_RESULT([$hip_want])
   fi

   AC_MSG_CHECKING([GPU architectures])
   HIPCC_ARCH=""
   for a in $hip_want ; do
      HIPCC_ARCH="$HIPCC_ARCH --offload-arch=$a"
   done
   AC_MSG_RESULT([$hip_want])

   dnl -rpath as well as -L, because a ROCm install does not necessarily put
   dnl its library directory on ldconfig's path: the stock rocm/dev container
   dnl does not, and the miner then compiles and links and dies at startup
   dnl with "libamdhip64.so.7: cannot open shared object file". Recording the
   dnl path in the binary beats needing LD_LIBRARY_PATH at every run.
   HIP_LDFLAGS="-L$hip_prefix/lib -Wl,-rpath,$hip_prefix/lib"
   dnl -lstdc++ for the same reason the CUDA side needs it: the kernels are
   dnl C++ and this miner links with CC.
   HIP_LIBS="-lamdhip64 -lstdc++"
   have_amd=true
fi

dnl Either vendor, or both. They are no longer alternatives: each builds its
dnl own library exporting the same avian_cuda_* ABI, and the miner loads them
dnl rather than linking them. That is what lets one binary drive an Nvidia
dnl card, an AMD card, or both in the same machine, and it is also why
dnl building for both does not make a binary that needs both drivers
dnl installed to start. See cuda/avian-gpu-backend.h.
dnl
dnl HAVE_CUDA keeps its name and now means no more than "this binary has GPU
dnl support". It gates the host-side code, every name in which is
dnl avian_cuda_* or avian_gpu_* whichever card ends up running it, and it is
dnl what cpu-miner.c, x16r-gate.c and the Windows build scripts already test.
dnl A rename that stopped at this one symbol would leave those testing
dnl something different from the sources.
if test x$have_nv = xtrue || test x$have_amd = xtrue ; then
   AC_DEFINE([HAVE_CUDA], [1], [Define to 1 to build a GPU X16RT backend.])
   have_gpu=true
fi

dnl -x hip because these files are named .cu, which hipcc otherwise hands to
dnl the host compiler as plain C++.
if test x$have_amd = xtrue ; then
   HIPCC_ARCH="$HIPCC_ARCH -x hip"
fi

AM_CONDITIONAL([HAVE_CUDA], [test x$have_gpu = xtrue])
AM_CONDITIONAL([HAVE_GPU_NV], [test x$have_nv = xtrue])
AM_CONDITIONAL([HAVE_GPU_AMD], [test x$have_amd = xtrue])
AC_SUBST(NVCC)
AC_SUBST(NVCC_ARCH)
AC_SUBST(CUDA_LDFLAGS)
AC_SUBST(CUDA_LIBS)
AC_SUBST(HIPCC)
AC_SUBST(HIPCC_ARCH)
AC_SUBST(HIP_LDFLAGS)
AC_SUBST(HIP_LIBS)

# libcurl install path (for mingw : --with-curl=/usr/local)
AC_ARG_WITH([curl],
   [  --with-curl=PATH         prefix where curl is installed [default=/usr]])

if test -n "$with_curl" ; then
   LIBCURL_CFLAGS="$LIBCURL_CFLAGS -I$with_curl/include"
   LIBCURL_CPPFLAGS="$LIBCURL_CPPFLAGS -I$with_curl/include"
   LIBCURL_LDFLAGS="-L$with_curl/lib $LIBCURL_LDFLAGS"
   LIBCURL="-lcurl -lz"
fi

CFLAGS="$CFLAGS $LIBCURL_CFLAGS"
CPPFLAGS="$CPPFLAGS $LIBCURL_CPPFLAGS"
LDFLAGS="$LDFLAGS $LIBCURL_LDFLAGS"

# AC_CHECK_LIB([curl], [curl_multi_timeout],
#    have_libcurl=yes,
#    have_libcurl=no AC_MSG_ERROR([curl library required])
# )

# LIBCURL_CHECK_CONFIG([yes], 7.15, curlconfig=yes, curlconfig=no)

AC_SUBST(LIBCURL)
AC_SUBST(LIBCURL_CFLAGS)
AC_SUBST(LIBCURL_CPPFLAGS)
# AC_SUBST(LIBCURL_LDFLAGS)

AC_SUBST(JANSSON_LIBS)
AC_SUBST(PTHREAD_FLAGS)
AC_SUBST(PTHREAD_LIBS)
AC_SUBST(WS2_LIBS)

AC_CONFIG_FILES([
	Makefile
	compat/Makefile
	compat/jansson/Makefile
	])
AC_OUTPUT

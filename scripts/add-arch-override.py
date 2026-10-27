#!/usr/bin/env python3
"""Add --with-cuda-arch, to build for one architecture while developing.

The detected list is Maxwell through Blackwell, nine targets over 31 files,
which measures at about fifty minutes. Right for a release, useless for a
change-compile-test loop, so the list needs an override.
"""
import io
import sys

p = sys.argv[1]
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()

if "with-cuda-arch" in s:
    print("already present")
    raise SystemExit(0)

old = """   AC_MSG_CHECKING([GPU architectures])
   NVCC_ARCH=""
   cuda_built=""
   cuda_newest=""
"""
new = """   dnl A release wants every architecture the toolkit can emit, which is nine
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
"""
if s.count(old) != 1:
    raise SystemExit("anchor not found")
s = s.replace(old, new, 1)

old_loop = "   for a in 52 61 75 80 86 89 100 120 ; do\n"
new_loop = "   for a in $cuda_want ; do\n"
if s.count(old_loop) != 1:
    raise SystemExit("arch loop not found")
s = s.replace(old_loop, new_loop, 1)

# The error message should name what was asked for, not a fixed list.
old_err = "      AC_MSG_ERROR([none of the expected architectures are supported by $NVCC])\n"
new_err = "      AC_MSG_ERROR([$NVCC supports none of: $cuda_want])\n"
if s.count(old_err) == 1:
    s = s.replace(old_err, new_err, 1)

io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
print("added --with-cuda-arch")

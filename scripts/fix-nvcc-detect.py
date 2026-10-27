#!/usr/bin/env python3
"""Replace AC_PATH_PROG's search-path form with an explicit check.

Passing a PATH as AC_PATH_PROG's fourth argument did not find
/usr/local/cuda-12.6/bin/nvcc even though it is there. Testing the prefix
directly is shorter, obvious, and cannot be got wrong by quoting.
"""
import io
import sys

p = sys.argv[1]
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()

old = '   AC_PATH_PROG([NVCC], [nvcc], [no], ["$cuda_prefix/bin$PATH_SEPARATOR$PATH"])\n'
new = ('''   dnl The prefix is checked directly rather than handed to AC_PATH_PROG as a
   dnl search path, which did not find an nvcc that was plainly there.
   AC_MSG_CHECKING([for nvcc])
   if test -x "$cuda_prefix/bin/nvcc" ; then
      NVCC="$cuda_prefix/bin/nvcc"
      AC_MSG_RESULT([$NVCC])
   else
      AC_MSG_RESULT([not under $cuda_prefix/bin])
      AC_PATH_PROG([NVCC], [nvcc], [no])
   fi
''')

if new in s:
    print("already fixed")
    raise SystemExit(0)
if s.count(old) != 1:
    raise SystemExit("expected 1 AC_PATH_PROG line, found %d" % s.count(old))
s = s.replace(old, new, 1)

# The error test has to cover an unset NVCC as well as the literal "no".
old_test = '   if test "x$NVCC" = "xno" ; then\n'
new_test = '   if test "x$NVCC" = "xno" || test "x$NVCC" = "x" ; then\n'
if s.count(old_test) == 1:
    s = s.replace(old_test, new_test, 1)

io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
print("fixed nvcc detection")

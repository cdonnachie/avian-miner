#!/usr/bin/env python3
"""Add --with-cuda to configure.ac and the nvcc rules to Makefile.am.

Modelled on the existing --with-avian-sv2 block, so the CUDA backend is off
unless asked for and a build without it is byte-identical to today's.
"""
import io
import os
import sys

root = sys.argv[1]


def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    for old, new, n in pairs:
        found = s.count(old)
        if found != n:
            raise SystemExit("%s: expected %d of %r, found %d" % (rel, n, old[:60], found))
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("  patched", rel)


# ---------------------------------------------------------------- configure.ac
CONFIGURE = r'''
dnl The CUDA GPU backend. Off unless asked for, so a build without it is
dnl unchanged.
AC_ARG_WITH([cuda],
   [  --with-cuda@<:@=PATH@:>@  build the CUDA X16RT backend; PATH is the toolkit prefix @<:@default=/usr/local/cuda@:>@])

have_cuda=false
if test "x$with_cuda" != "x" && test "x$with_cuda" != "xno" ; then
   if test "x$with_cuda" = "xyes" ; then
      cuda_prefix=/usr/local/cuda
   else
      cuda_prefix="$with_cuda"
   fi
   AC_PATH_PROG([NVCC], [nvcc], [no], ["$cuda_prefix/bin$PATH_SEPARATOR$PATH"])
   if test "x$NVCC" = "xno" ; then
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

   AC_MSG_CHECKING([GPU architectures])
   NVCC_ARCH=""
   cuda_built=""
   cuda_newest=""
   dnl 52 and 61 are Maxwell and Pascal: a 900 or 10 series card. They cost
   dnl about five minutes of build and seven megabytes, measured, and the
   dnl X16RT tuning tables upstream are full of 1060s and 1080s, so they stay
   dnl while any toolkit still offers them.
   for a in 52 61 75 80 86 89 100 120 ; do
      for h in $cuda_avail ; do
         if test "x$a" = "x$h" ; then
            NVCC_ARCH="$NVCC_ARCH -gencode=arch=compute_$a,code=sm_$a"
            cuda_built="$cuda_built $a"
            cuda_newest="$a"
         fi
      done
   done
   if test "x$NVCC_ARCH" = "x" ; then
      AC_MSG_ERROR([none of the expected architectures are supported by $NVCC])
   fi
   dnl Also emit PTX for the newest architecture this toolkit knows, so a card
   dnl released after the build still runs by driver JIT instead of failing to
   dnl launch at all.
   NVCC_ARCH="$NVCC_ARCH -gencode=arch=compute_$cuda_newest,code=compute_$cuda_newest"
   AC_MSG_RESULT([sm$cuda_built, plus compute_$cuda_newest as PTX])

   CUDA_LDFLAGS="-L$cuda_prefix/lib64 -L$cuda_prefix/lib"
   dnl -lstdc++ because the kernels are C++ while this miner links with CC.
   CUDA_LIBS="-lcudart -lstdc++"
   AC_DEFINE([HAVE_CUDA], [1], [Define to 1 to build the CUDA X16RT backend.])
   have_cuda=true
fi
AM_CONDITIONAL([HAVE_CUDA], [test x$have_cuda = xtrue])
AC_SUBST(NVCC)
AC_SUBST(NVCC_ARCH)
AC_SUBST(CUDA_LDFLAGS)
AC_SUBST(CUDA_LIBS)
'''

anchor = "AC_SUBST(AVIAN_SV2_CFLAGS)\nAC_SUBST(AVIAN_SV2_LIBS)\n"
p = os.path.join(root, "configure.ac")
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
if "with-cuda" in s:
    print("  configure.ac already patched")
else:
    edit("configure.ac", [(anchor, anchor + CONFIGURE, 1)])

# The gate registration needs declaring where algo_gate_t is in scope.

# ----------------------------------------------------------------- Makefile.am
cu = []
for base, _, files in os.walk(os.path.join(root, "cuda", "upstream")):
    for f in sorted(files):
        if f.endswith(".cu"):
            cu.append(os.path.relpath(os.path.join(base, f), root).replace(os.sep, "/"))
cu.sort()
sources = ["cuda/device.cu", "cuda/avian-cuda-support.cu",
           "algo/x16/x16rt-cuda-batch.cu"] + cu

MAKEFILE = """
if HAVE_CUDA
# automake has no notion of .cu, so these are compiled by an explicit rule and
# handed to the link as objects. Listed one per line rather than globbed, so
# that what is built is what is in the tree and `make dist` sees them.
CUDA_SOURCES = \\
%s

CUDA_OBJECTS = $(CUDA_SOURCES:.cu=.o)

# -I. and -I$(top_srcdir) find cpuminer-config.h, miner.h and compat.h;
# cuda/ finds the shim; cuda/upstream/ finds the vendored headers at the paths
# upstream's own includes expect.
NVCC_INCLUDES = -I. -I$(top_srcdir) -I$(top_srcdir)/cuda -I$(top_srcdir)/cuda/upstream

# -w because the vendored kernels are not ours to make warning clean, and the
# noise would bury warnings from code that is.
NVCC_FLAGS = @NVCC_ARCH@ -std=c++14 -w $(NVCC_INCLUDES)

.cu.o:
	@NVCC@ $(NVCC_FLAGS) -c -o $@ $<

avian_miner_LDADD += $(CUDA_OBJECTS) @CUDA_LDFLAGS@ @CUDA_LIBS@
avian_miner_SOURCES += algo/x16/x16rt-cuda-gate.c \
                       algo/x16/x16rt-cuda-host.c
EXTRA_DIST = $(CUDA_SOURCES)
CLEANFILES = $(CUDA_OBJECTS)
endif
""" % " \\\n".join("  " + s for s in sources)

p = os.path.join(root, "Makefile.am")
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
if "HAVE_CUDA" in s:
    # The block is committed now, so this is the common path and the list in
    # it is frozen. That is the point: what gets built is what is written
    # down. But a .cu added to cuda/upstream would then go unbuilt with
    # nothing to say so, and a kernel that is not compiled fails at link if
    # you are lucky and at runtime if you are not. So the committed list is
    # checked against the tree every time.
    listed = set()
    for line in s.split("CUDA_SOURCES = ", 1)[1].splitlines():
        if not line.strip():
            break
        name = line.strip().rstrip("\\").strip()
        if name.endswith(".cu"):
            listed.add(name)
    want = set(sources)
    if listed != want:
        missing = sorted(want - listed)
        extra = sorted(listed - want)
        for f in missing:
            print("  NOT IN Makefile.am: %s" % f)
        for f in extra:
            print("  LISTED BUT ABSENT:  %s" % f)
        raise SystemExit("Makefile.am's CUDA_SOURCES no longer matches the tree")
    print("  Makefile.am already patched, %d sources, list matches the tree"
          % len(listed))
else:
    # SUFFIXES must be set before the rule is used, and near the top is the
    # conventional place.
    suffix_anchor = "SUBDIRS = compat\n"
    if s.count(suffix_anchor) != 1:
        raise SystemExit("Makefile.am: SUBDIRS anchor not found")
    s = s.replace(suffix_anchor, "SUFFIXES = .cu\n\n" + suffix_anchor, 1)
    s = s.rstrip("\n") + "\n" + MAKEFILE
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("  patched Makefile.am (%d CUDA sources)" % len(sources))

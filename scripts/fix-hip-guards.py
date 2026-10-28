#!/usr/bin/env python3
"""Stop the host-side blockDim/threadIdx declarations reaching a HIP build.

Both helper headers carry this, for the benefit of a host compiler that has
never heard of a thread index:

    #ifndef __CUDA_ARCH__
    // define blockDim and threadIdx for host
    extern const dim3 blockDim;
    extern const uint3 threadIdx;
    #endif

Under nvcc's device pass __CUDA_ARCH__ is defined, so it is skipped. hipcc
never defines __CUDA_ARCH__, which is the same fact that hands us the
guarded plain-C fallbacks for free elsewhere, and here it works against us:
the declarations are always compiled and collide with HIP's own builtins.
Measured with HIP 7.2, that is 62 of 357 error reports, from four lines.

    error: redeclaration of 'blockDim' with a different type:
           'const dim3' vs 'const __hip_builtin_blockDim_t'

So the guard is widened to mean what it was always for: a host compiler, and
not merely a compiler that is not nvcc's device pass. Nothing changes for
CUDA, where __HIPCC__ is undefined and the condition is as it was.

Idempotent, and re-applied at staging time like the other patches here,
because cuda/upstream is vendored and a re-vendor would drop this with no
build failing to say so.
"""
import io
import os
import sys

OLD = """#ifndef __CUDA_ARCH__
// define blockDim and threadIdx for host
extern const dim3 blockDim;
extern const uint3 threadIdx;
#endif"""

NEW = """/* __HIPCC__ as well as __CUDA_ARCH__: hipcc never defines the latter, so
 * without this these host declarations are compiled into a device build and
 * collide with HIP's own blockDim and threadIdx. See
 * scripts/fix-hip-guards.py. */
#if !defined(__CUDA_ARCH__) && !defined(__HIPCC__)
// define blockDim and threadIdx for host
extern const dim3 blockDim;
extern const uint3 threadIdx;
#endif"""

FILES = ["cuda_helper.h", "cuda_helper_alexis.h"]

# hamsi512 is the same fault in a different shape. It defines its 64x16 table
# twice, under #if defined(__CUDA_ARCH__) as __constant__ and under #else as
# a plain host array. HIP takes the #else, and then the kernel refers to a
# host variable:
#
#   error: reference to __host__ variable 'd_T512' in __global__ function
#
# A single __constant__ definition visible to both passes is how HIP code is
# normally written, so the device branch is the right one to take.
TABLE_FILE = os.path.join("x13", "cuda_x13_hamsi512.cu")
TABLE_OLD = "#if defined(__CUDA_ARCH__)\n"
TABLE_NEW = ("/* || __HIPCC__ so the __constant__ table, not the host copy, is the one\n"
             " * a HIP build sees. See scripts/fix-hip-guards.py. */\n"
             "#if defined(__CUDA_ARCH__) || defined(__HIPCC__)\n")


def patch_table(root):
    """The hamsi512 constant table. Returns 1 if changed, 0 if already done."""
    p = os.path.join(root, TABLE_FILE)
    if not os.path.exists(p):
        print("  not present, skipped: %s" % TABLE_FILE)
        return 0
    s = io.open(p, encoding="utf-8", errors="surrogateescape",
                newline="").read()
    if "defined(__CUDA_ARCH__) || defined(__HIPCC__)" in s:
        return 0
    n = s.count(TABLE_OLD)
    if n != 1:
        raise SystemExit(
            "%s: expected 1 __CUDA_ARCH__ guard, found %d" % (TABLE_FILE, n))
    s = s.replace(TABLE_OLD, TABLE_NEW, 1)
    io.open(p, "w", encoding="utf-8", errors="surrogateescape",
            newline="").write(s)
    print("  widened the table guard in %s" % TABLE_FILE)
    return 1


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "cuda/upstream"
    done = 0
    already = 0

    for name in FILES:
        p = os.path.join(root, name)
        if not os.path.exists(p):
            print("  not present, skipped: %s" % name)
            continue
        s = io.open(p, encoding="utf-8", errors="surrogateescape",
                    newline="").read()
        if "!defined(__HIPCC__)" in s:
            already += 1
            continue
        n = s.count(OLD)
        if n != 1:
            raise SystemExit(
                "%s: expected 1 guard block, found %d" % (name, n))
        s = s.replace(OLD, NEW, 1)
        io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                newline="").write(s)
        print("  widened the guard in %s" % name)
        done += 1

    done += patch_table(root)

    if already and not done:
        print("  %d file(s) already guarded against HIP" % already)
    return 0


if __name__ == "__main__":
    sys.exit(main())

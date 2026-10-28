#!/usr/bin/env python3
"""Stop HIP rotating a uint2 one 32-bit half at a time.

The worst defect of the port, because it compiled, ran, and produced wrong
hashes rather than failing.

Both helper headers end their rotate definitions with an untyped fallback for
whatever is not a CUDA device compile:

    #define ROTL64(x, n)  (((x) << (n)) | ((x) >> (64 - (n))))

hipcc never defines __CUDA_ARCH__, so every HIP build takes that branch. For
a uint64_t it is a correct rotate. For a uint2 it is not even close: CUDA's
uint2 is a plain struct with no operators, so this would fail to compile,
while HIP's is a class template whose << and >> are element-wise. The macro
then rotates each 32-bit half on its own, losing the carry between them, and
`>> (64 - n)` on a 32-bit element shifts past its width into the bargain.

Measured on gfx942 against a rotate written from its own definition:

    ROTL64 over uint64_t :  0 of 64 wrong
    ROTL64 over uint2    : 59 of 64 wrong

    n=1  got 490dc185581cd094  want 490dc184581cd095
                   ^        ^              ^        ^

and the bit that moves is exactly the one that should have crossed between
the halves.

skein holds its state in uint2 and bmw reaches the same macro through its
uint2 paths, which is why the stage bisect put the first divergence on those
two and on no others.

The fix is typed overloads rather than a wider macro: a uint2 is routed
through its 64-bit value, and anything else the headers are handed now fails
to compile instead of being silently reinterpreted. Only the HIP branch
changes; CUDA keeps the macro it has always had, and so does a host compile.

The 64-bit halves are assembled by hand rather than through vectorize() and
devectorize(), because cuda_helper.h declares both of those below the macros
it would need them in.

Idempotent, and re-applied at staging time, because cuda/upstream is vendored
and a re-vendor would drop this. Nothing would fail to build if it did. The
miner would simply go back to hashing incorrectly.
"""
import io
import os
import re
import sys

MARKER = "AVIAN_HIP_ROTATE"

TEMPLATE = '''#if defined(__HIPCC__)
/* %(marker)s: typed, because HIP's uint2 is a class whose << and >>
 * are element-wise, so the macro below rotates each half separately and
 * drops the carry between them. See scripts/fix-hip-uint2-rotate.py. */
static __host__ __device__ __forceinline__
uint64_t %(name)s(const uint64_t x, const int n)
{
\treturn %(expr64)s;
}
static __host__ __device__ __forceinline__
uint2 %(name)s(const uint2 v, const int n)
{
\tconst uint64_t x = ((uint64_t)v.y << 32) | (uint64_t)v.x;
\tconst uint64_t r = %(expr64)s;
\tuint2 out;
\tout.x = (uint32_t)(r & 0xffffffffULL);
\tout.y = (uint32_t)(r >> 32);
\treturn out;
}
#else
%(original)s
#endif
'''

#: name -> the 64-bit expression, written once so the two overloads cannot
#: drift apart.
EXPR = {
    "ROTL64": "(x << n) | (x >> (64 - n))",
    "ROTR64": "(x >> n) | (x << (64 - n))",
}

MACRO = re.compile(r"^#define\s+(ROTL64|ROTR64)\(x, n\)\s+.*$")

FILES = ["cuda_helper.h", "cuda_helper_alexis.h"]


def patch_file(p):
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    if MARKER in s:
        return 0
    lines = s.splitlines(True)
    out = []
    n = 0
    for line in lines:
        m = MACRO.match(line.rstrip("\r\n"))
        if m:
            name = m.group(1)
            out.append(TEMPLATE % {
                "marker": MARKER,
                "name": name,
                "expr64": EXPR[name],
                "original": line.rstrip("\r\n"),
            })
            n += 1
            continue
        out.append(line)
    if n:
        io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                newline="").write("".join(out))
    return n


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "cuda/upstream"
    total = 0
    present = 0
    for f in FILES:
        p = os.path.join(root, f)
        if not os.path.exists(p):
            print("  not present, nothing to do: %s" % p)
            continue
        present += 1
        n = patch_file(p)
        if n:
            print("  %s: %d rotate macro(s) given typed HIP overloads" % (f, n))
            total += n

    if total:
        print("  %d rotate(s) no longer reinterpret a uint2" % total)
    elif present:
        print("  the rotates already have typed HIP overloads")
    return 0


if __name__ == "__main__":
    sys.exit(main())

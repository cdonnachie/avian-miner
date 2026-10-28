#!/usr/bin/env python3
"""Give HIP an int overload beside every uint2-and-scalar operator.

The defect that made skein and bmw hash incorrectly on gfx942 while nine
other kernels were fine, and the hardest of the port to see, because the
nearby code is correct and the wrong call looks identical to the right one.

skein writes, among a dozen similar lines:

    hash64[5] = h5 + 8;          /* h5 is p[5], a uint2 */

Both helper headers define the operator that is meant to serve this:

    static __device__ __forceinline__
    uint2 operator+ (const uint2 a, const uint32_t b)
    { return vectorize(devectorize(a) + b); }

On CUDA that is the only viable candidate, because uint2 is a plain struct
and 8 has nowhere else to go. On HIP uint2 is a class template with a
converting constructor from a scalar, so `8` can become uint2(8, 8) and the
element-wise operator+ takes the call instead. An int literal is not an exact
match for the uint32_t overload, so the two compete and the wrong one wins.

Measured on gfx942, with a uint64_t reference:

    uint2 + 8   (int literal) : 64 of 64 wrong
    uint2 + eight (uint32_t)  :  0 of 64 wrong

Note the second line. A scalar held in a uint32_t is an exact match and
resolves correctly, which is why an earlier test of "uint2 plus a scalar"
passed and cleared this operator wrongly. Only the literal form is affected.

The fix forwards rather than reimplements: the int overload casts to
uint32_t and calls the existing one, which is precisely the conversion CUDA
performs implicitly, including for a negative value. So the two targets agree
by construction rather than by a second copy of the arithmetic.

Idempotent, and re-applied at staging time, because cuda/upstream is vendored
and a re-vendor would drop it. Nothing would fail to build. The miner would
simply go back to hashing incorrectly on AMD.
"""
import io
import os
import re
import sys

MARKER = "AVIAN_HIP_UINT2_SCALAR"

#: Every `uint2 operator?(const uint2, const uint32_t)` in the vendored
#: headers, whatever spacing it was written with.
SIG = re.compile(
    r"static\s+__device__\s+__forceinline__\s+uint2\s+operator\s*([-+^])\s*"
    r"\(\s*const\s+uint2\s+(\w+)\s*,\s*const\s+uint32_t\s+(\w+)\s*\)")

FILES = ["cuda_vectors.h", "cuda_helper.h", "cuda_helper_alexis.h",
         "cuda_vectors_alexis.h"]

TEMPLATE = '''
#if defined(__HIPCC__)
/* %(marker)s: an int literal is not an exact match for the uint32_t
 * overload above, and HIP's uint2 can be constructed from a scalar, so
 * without this `x %(op)s 8` builds a uint2(8, 8) and operates on each half.
 * Forwards rather than reimplements, so the two cannot disagree. See
 * scripts/fix-hip-uint2-scalar.py. */
static __device__ __forceinline__
uint2 operator%(op)s (const uint2 %(a)s, const int %(b)s)
{
\treturn %(a)s %(op)s (uint32_t)%(b)s;
}
#endif
'''


def end_of_definition(s, at):
    """Index just past the closing brace of the body that follows `at`."""
    i = s.index("{", at)
    depth = 0
    while i < len(s):
        if s[i] == "{":
            depth += 1
        elif s[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise SystemExit("unterminated operator body")


def patch_file(p):
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    if MARKER in s:
        return 0
    out = []
    pos = 0
    n = 0
    for m in SIG.finditer(s):
        end = end_of_definition(s, m.end())
        out.append(s[pos:end])
        out.append(TEMPLATE % {"marker": MARKER, "op": m.group(1),
                               "a": m.group(2), "b": m.group(3)})
        pos = end
        n += 1
    if n:
        out.append(s[pos:])
        io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                newline="").write("".join(out))
    return n


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "cuda/upstream"
    total = 0
    seen = 0
    for f in FILES:
        p = os.path.join(root, f)
        if not os.path.exists(p):
            continue
        seen += 1
        n = patch_file(p)
        if n:
            print("  %s: %d scalar operator(s) given an int overload" % (f, n))
            total += n

    if total:
        print("  %d operator(s) no longer broadcast an int literal" % total)
    elif seen:
        print("  the scalar operators already have int overloads")
    else:
        print("  not present, nothing to do: %s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Plain-C bodies for the last eight PTX expressions, selected by AVIAN_NO_PTX.

What is left after make-ptx-portable.py (lop3) and fix-xor-asm.py (xor.b64):
a 64-bit majority function built from and.b64/or.b64, two shr.b64, two
shl.b64, the two mov.b64 halves of a doubleword swap, and one
ld.global.nc.v4.u32. HIP rejects every one of them with

    error: invalid output constraint '=l' in asm

because 'l' is a PTX register class and means nothing to an AMD target.

Six of the seven functions carry upstream's own plain-C equivalent directly
underneath, commented out:

    asm("shr.b64 %0,%1,%2;\\n\\t" : "=l"(result) : "l"(x), "r"(n));
    return result;
//  return x >> n;

so the portable body is upstream's, not a reimplementation, and the risk is
transcription rather than translation. andor is the one worth stating in
full, since it is the only site here whose asm is more than one operation:

    m = a & b;  n = a | b;  result = n & c;  result = result | m

which is ((a | b) & c) | (a & b), and that is exactly the expression upstream
left commented beneath it.

expanded_vector is the exception with no commented form, only a hint one line
above: `//#define expanded_vector(x) __ldg(&g_fft4[x])`. ld.global.nc is the
non-coherent load __ldg emits, so the portable body uses __ldg rather than a
plain dereference, which would drop the cache hint on the CUDA side of the
switch as well.

Idempotent per function rather than per file, because this file already
contains AVIAN_NO_PTX from the lop3 and xor passes, and re-applied at staging
time, because cuda/upstream is vendored and a re-vendor would drop this with
no build failing to say so.
"""
import io
import os
import re
import sys

MARKER = "AVIAN_NO_PTX"

# (file, return type, function, portable body lines, the mnemonics the asm is
# allowed to contain). The mnemonic set is the check that the body is the one
# this entry describes: if a re-vendor brings a different implementation under
# the same name, it is refused rather than silently given the wrong body.
SITES = [
    ("cuda_helper_alexis.h", "uint64_t", "andor",
     ["return ((a | b) & c) | (a & b);"],
     {"and.b64", "or.b64"}),
    ("cuda_helper_alexis.h", "uint64_t", "shr_u64",
     ["return x >> n;"],
     {"shr.b64"}),
    ("cuda_helper_alexis.h", "uint64_t", "shl_u64",
     ["return x << n;"],
     {"shl.b64"}),
    # mov.b64 {lo,hi}, v then mov.b64 v, {hi,lo}: the two words change places,
    # which is a 32-bit rotation of the doubleword.
    ("cuda_helper_alexis.h", "uint64_t", "SWAPDWORDS",
     ["return (value >> 32) | (value << 32);"],
     {"mov.b64"}),
    ("cuda_helper_alexis.h", "uint2", "SHL2",
     ["return vectorize(devectorize(a) << n);"],
     {"shl.b64"}),
    ("cuda_helper_alexis.h", "uint2", "SHR2",
     ["return vectorize(devectorize(a) >> n);"],
     {"shr.b64"}),
    (os.path.join("x11", "cuda_x11_simd512_func.cuh"), "void", "expanded_vector",
     ["const uint4 v = __ldg(ptr);",
      "w[0] = v.x; w[1] = v.y; w[2] = v.z; w[3] = v.w;"],
     {"ld.global.nc.v4.u32"}),

    # The five below were invisible to -fsyntax-only, which is the reason this
    # script has a second half. HIP rejects 'l' outright, so every 64-bit site
    # announced itself; it accepts 'r' and then fails in instruction
    # selection, so a 32-bit site compiles as far as codegen and no further.
    # Found by compiling to object code rather than checking syntax.

    # bfe.u32 d,a,b,c extracts c bits of a starting at b. A length of zero
    # gives zero, and 1u << 32 is undefined in C, so both edges are written
    # out rather than left to the shift.
    ("cuda_helper_alexis.h", "uint32_t", "bfe",
     ["const uint32_t mask = (numBits >= 32) ? 0xFFFFFFFFu",
      "                                      : ((1u << numBits) - 1u);",
      "return (bit >= 32) ? 0u : ((x >> bit) & mask);"],
     {"bfe.u32"}),

    # bfi.b32 d,a,b,c,d replaces c-length bits of b at position d with the low
    # bits of a. Here the arguments are (x inserted into a at bit, numBits).
    ("cuda_helper_alexis.h", "uint32_t", "bfi",
     ["if (bit >= 32 || numBits == 0) return a;",
      "const uint32_t width = (numBits >= 32) ? 0xFFFFFFFFu",
      "                                       : ((1u << numBits) - 1u);",
      "const uint32_t mask = width << bit;",
      "return (a & ~mask) | ((x << bit) & mask);"],
     {"bfi.b32"}),

    # Three bit permutations, written as PTX because of the last instruction:
    # vshl.clamp.add is (x << n) + y in one go, an Nvidia video instruction
    # with no AMD counterpart. The asm reads
    #     y = x & HIGH; x ^= y; y >>= n; x = (x << n) + y
    # so x ends as ((x & LOW) << n) | ((x & HIGH) >> n), the two halves
    # landing in disjoint bits, which is a swap of adjacent n-bit groups and
    # exactly what each function is named for.
    (os.path.join("quark", "cuda_jh512.cu"), "void", "SWAP4x4",
     ["for (uint32_t i = 0; i < 4; i++, ++x)",
      "\t*x = ((*x & 0x0F0F0F0Fu) << 4) | ((*x & 0xF0F0F0F0u) >> 4);"],
     {"and.b32", "xor.b32", "shr.b32", "vshl.u32.u32.u32.clamp.add"}),
    (os.path.join("quark", "cuda_jh512.cu"), "void", "SWAP2x4",
     ["for (uint32_t i = 0; i < 4; i++, ++x)",
      "\t*x = ((*x & 0x33333333u) << 2) | ((*x & 0xCCCCCCCCu) >> 2);"],
     {"and.b32", "xor.b32", "shr.b32", "vshl.u32.u32.u32.clamp.add"}),
    (os.path.join("quark", "cuda_jh512.cu"), "void", "SWAP1x4",
     ["for (uint32_t i = 0; i < 4; i++, ++x)",
      "\t*x = ((*x & 0x55555555u) << 1) | ((*x & 0xAAAAAAAAu) >> 1);"],
     {"and.b32", "xor.b32", "shr.b32", "vshl.u32.u32.u32.clamp.add"}),
]


HEADER = '#include "avian-ptx-portable.h"'
HEADER_NOTE = ("/* AVIAN_NO_PTX and avian_lop3. "
               "See scripts/make-ptx-portable.py. */")


def add_header(s):
    """Put the portable header after the file's last leading #include.

    The same placement make-ptx-portable.py uses, and deliberately the same
    two lines, so a file that both passes touch does not end up with the
    include written two different ways.
    """
    lines = s.splitlines(True)
    last = -1
    for n, line in enumerate(lines[:100]):
        if line.lstrip().startswith("#include"):
            last = n
    at = last + 1 if last >= 0 else 0
    return "".join(lines[:at] + [HEADER_NOTE + "\n", HEADER + "\n"] + lines[at:])


def mnemonics_in(body):
    """PTX mnemonics appearing in the body's string literals.

    Restricted to quoted text on purpose: the surrounding C contains things
    like `temp.x` that match the same shape as `mov.b64`.
    """
    quoted = " ".join(re.findall(r'"([^"]*)"', body))
    return set(re.findall(r"\b([a-z][a-z0-9]*(?:\.[a-z0-9]+)+)\b", quoted))


def patch_one(s, ret, name, lines, allowed):
    """Wrap one function's body. Returns (new_s, 'done' | 'already')."""
    # The brace sits on its own line for some of these and at the end of the
    # signature for others, so both are accepted. The body ends at the first
    # closing brace in column 0, which holds because each of these functions
    # is written at file scope with no nested block at that indent.
    pat = re.compile(
        r"(\b" + ret + r"\s+" + name + r"\s*\([^)]*\)\s*\{\n)(.*?)(\n\})",
        re.DOTALL)
    found = pat.findall(s)
    if not found:
        raise SystemExit("%s: signature and body not found" % name)
    if len(found) > 1:
        raise SystemExit("%s: %d definitions, cannot tell which to patch"
                         % (name, len(found)))

    m = pat.search(s)
    head, old_body, tail = m.group(1), m.group(2), m.group(3)
    if MARKER in old_body:
        return s, "already"

    seen = mnemonics_in(old_body)
    if seen != allowed:
        raise SystemExit(
            "%s: asm contains %s, expected %s; refusing to substitute a body "
            "for an implementation this script does not recognise"
            % (name, ", ".join(sorted(seen)) or "no mnemonic",
               ", ".join(sorted(allowed))))

    new_body = ("#if " + MARKER + "\n"
                + "".join("\t" + ln + "\n" for ln in lines)
                + "#else\n"
                + old_body + "\n"
                "#endif")
    return s[:m.start()] + head + new_body + tail + s[m.end():], "done"


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "cuda/upstream"
    by_file = {}
    for rel, ret, name, lines, allowed in SITES:
        by_file.setdefault(rel, []).append((ret, name, lines, allowed))

    done = already = 0
    for rel, entries in by_file.items():
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            print("  not present, nothing to do: %s" % p)
            continue

        s = io.open(p, encoding="utf-8", errors="surrogateescape",
                    newline="").read()
        # AVIAN_NO_PTX is only meaningful where the header that defines it has
        # been included. make-ptx-portable.py adds it to the files it patches,
        # but cuda_jh512.cu has no lop3 site and so was never visited by it;
        # inserting it here rather than demanding that script have run first
        # keeps the two passes independent.
        if "avian-ptx-portable.h" not in s:
            s = add_header(s)
            print("  %s: added the portable header" % rel)

        before = s
        for ret, name, lines, allowed in entries:
            s, how = patch_one(s, ret, name, lines, allowed)
            if how == "done":
                print("  %s: %s" % (name, lines[0]))
                done += 1
            else:
                already += 1

        if s != before:
            io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write(s)

    if done:
        print("  %d expression(s) now have a portable form" % done)
    if already:
        print("  %d already portable" % already)
    return 0


if __name__ == "__main__":
    sys.exit(main())

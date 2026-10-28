#!/usr/bin/env python3
"""Plain-C bodies for the xor.b64 helpers, selected by AVIAN_NO_PTX.

cuda_helper_alexis.h implements xor1, xor3, xor5, xor8 and xor9 as chains of
inline PTX:

    asm("xor.b64 %0, %1, %2;" : "=l"(result) : "l"(g) ,"l"(h));
    asm("xor.b64 %0, %0, %1;" : "+l"(result) : "l"(f));
    ...

Twenty-one such statements, and they were the entire remaining HIP wall once
the lop3 work and the parse errors were out of the way. HIP reports them as

    error: invalid output constraint '+l' in asm

because 'l' is a PTX register class and means nothing to an AMD target.

Unlike lop3, there is nothing to decode here. These are 64-bit exclusive ors,
written as assembly for an operation every compiler already emits optimally,
so the portable body is the expression itself and is not a reimplementation
of anything. The asm is kept under #else so the CUDA build is untouched,
exactly as with the lop3 sites, although in this case there is no reason to
believe it is faster.

Idempotent, and re-applied at staging time, because cuda/upstream is
vendored and a re-vendor would drop this with no build failing to say so.
"""
import io
import os
import re
import sys

# Each entry is (function name, parameters in xor order). The body is
# generated from the parameter list, so the expression cannot disagree with
# the signature.
FUNCS = [
    ("xor1", "a b"),
    ("xor3", "a b c"),
    ("xor5", "a b c d e"),
    ("xor9", "a b c d e f g h i"),
    ("xor8", "a b c d e f g h"),
]

MARKER = "AVIAN_NO_PTX"


def body_for(params):
    return " ^ ".join(params.split())


def patch_one(s, name, params):
    """Wrap one function's body. Returns (new_s, n_asm_statements)."""
    # From the opening brace after the signature to the closing brace at
    # column 0. The sources put each of these functions on its own, so the
    # first line starting with } ends it.
    pat = re.compile(
        r"(uint64_t\s+" + name + r"\s*\([^)]*\)\s*\n\{\n)(.*?)(\n\})",
        re.DOTALL)
    m = pat.search(s)
    if not m:
        raise SystemExit("%s: signature and body not found" % name)

    head, old_body, tail = m.group(1), m.group(2), m.group(3)
    n = old_body.count("xor.b64")
    if n == 0:
        raise SystemExit("%s: no xor.b64 in the body" % name)
    # Not one asm statement per xor: xor3 puts two xor.b64 in a single
    # multi-line asm block. What has to hold is that the body does nothing
    # but exclusive ors, so every mnemonic in it is checked instead of the
    # statements being counted.
    mnemonics = set(re.findall(r'asm\(\s*"\s*([a-z0-9_.]+)', old_body))
    stray = mnemonics - {"xor.b64"}
    if stray:
        raise SystemExit("%s: body also contains %s; not a plain xor chain"
                         % (name, ", ".join(sorted(stray))))
    if len(re.findall(r"xor\.b64", old_body)) != n:
        raise SystemExit("%s: inconsistent xor.b64 count" % name)

    new_body = ("#if " + MARKER + "\n"
                "\treturn " + body_for(params) + ";\n"
                "#else\n"
                + old_body + "\n"
                "#endif")
    return s[:m.start()] + head + new_body + tail + s[m.end():], n


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "cuda/upstream"
    p = os.path.join(root, "cuda_helper_alexis.h")
    if not os.path.exists(p):
        print("  not present, nothing to do: %s" % p)
        return 0

    s = io.open(p, encoding="utf-8", errors="surrogateescape",
                newline="").read()
    if MARKER in s and "return a ^ b;" in s:
        print("  the xor helpers already have plain-C bodies")
        return 0

    # The include has to be there for AVIAN_NO_PTX to mean anything. It is
    # already added by make-ptx-portable.py for the lop3 sites in this same
    # file, so this only has to check rather than insert.
    if "avian-ptx-portable.h" not in s:
        raise SystemExit(
            "cuda_helper_alexis.h does not include avian-ptx-portable.h; "
            "run make-ptx-portable.py first")

    total = 0
    for name, params in FUNCS:
        s, n = patch_one(s, name, params)
        print("  %s: %d xor.b64 -> %s" % (name, n, body_for(params)))
        total += n

    io.open(p, "w", encoding="utf-8", errors="surrogateescape",
            newline="").write(s)
    print("  %d xor.b64 statement(s) now have a portable form" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Give every lop3.b32 site a plain-C form, selected by AVIAN_NO_PTX.

Run after re-vendoring the CUDA kernels. Upstream does not carry this, and a
direct edit to cuda/upstream would be lost the next time the tree is
refreshed, which is why this is a script and not a patch.

Why
---
PTX is Nvidia's. Every asm block in the vendored tree is a wall between these
kernels and AMD or anyone else, and lop3.b32 is 60 of them, the largest
single group by a wide margin. The helper headers already carry portable
fallbacks for their rotates and xors behind __CUDA_ARCH__ guards, so those
come for free under hipcc, which does not define it. lop3 does not: most of
its sites are unguarded, and the rest are guarded in a way that still picks
the asm on anything that defines __CUDA_ARCH__.

How, and why it is not a transcription job
------------------------------------------
PTX defines lop3's immediate as a truth table over three inputs, indexed by
(a << 2) | (b << 1) | c. So each site already carries its own definition, and
avian_lop3 in cuda/avian-ptx-portable.h evaluates it generically. Hand-
writing 60 boolean expressions would be 60 chances to put a wrong bit in a
hash; decoding the immediate is correct by construction at every site.

The vendored sources confirm the convention themselves. cuda_helper.h
annotates its commonest immediate as "0x96 = 0xF0 ^ 0xCC ^ 0xAA", and 0xF0,
0xCC and 0xAA are exactly the tables selecting a, b and c.

Two shapes occur, and both are asserted rather than assumed:

    asm("lop3.b32 %0, %1, %2, %3, LUT;" : "=r"(d) : "r"(a), "r"(b), "r"(c));
    asm("lop3.b32 %0, %0, %1, %2, LUT;" : "+r"(d) : "r"(b), "r"(c));

the second being an in-place form where the destination is also the first
input. Operands are arbitrary C, including things like -(round & 0x04) >> 2,
so they are extracted by balanced-paren scanning rather than by a regex that
would stop at the first inner bracket.

Verifying it
------------
The point of keeping both paths is that the portable one is testable on the
hardware we have. Build with -DAVIAN_NO_PTX=1 and run the forced-order stage
bisect: every stage of a chain reporting 0 differing is direct evidence that
the plain-C forms are hash-identical to the PTX. A port that skipped this
would rest on a reading of the ISA rather than a measurement, and this
project has twice paid for trusting that a thing compiles.
"""
import io
import os
import sys

HEADER = '#include "avian-ptx-portable.h"'

HEADER_NOTE = ("/* AVIAN_NO_PTX and avian_lop3. "
               "See scripts/make-ptx-portable.py. */")

# The two operand layouts, as spelled in the PTX template. Anything else is
# refused rather than guessed at.
FORM_OUT = "%0, %1, %2, %3"
FORM_INPLACE = "%0, %0, %1, %2"


def match_paren(s, i):
    """Index of the ')' closing the '(' at i, ignoring string literals."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c == '"':
            i += 1
            while i < len(s) and s[i] != '"':
                i += 2 if s[i] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def operands(s):
    """Every "=r"/"+r"/"r" constraint in order, as (kind, expression)."""
    out = []
    i = 0
    while True:
        j = s.find('"', i)
        if j < 0:
            return out
        k = s.find('"', j + 1)
        if k < 0:
            return out
        kind = s[j + 1:k]
        rest = s[k + 1:]
        lead = len(rest) - len(rest.lstrip())
        if kind in ("=r", "+r", "r") and rest[lead:lead + 1] == "(":
            start = k + 1 + lead
            end = match_paren(s, start)
            if end < 0:
                return out
            out.append((kind, s[start + 1:end].strip()))
            i = end + 1
        else:
            i = k + 1


def rewrite(line):
    """The plain-C assignment for one lop3 line, or None if unrecognised."""
    at = line.find("asm")
    if at < 0:
        return None
    open_at = line.find("(", at)
    if open_at < 0:
        return None
    close_at = match_paren(line, open_at)
    if close_at < 0:
        return None

    inner = line[open_at + 1:close_at]
    q0 = inner.find('"')
    q1 = inner.find('"', q0 + 1)
    if q0 < 0 or q1 < 0:
        return None
    template = inner[q0 + 1:q1]

    if FORM_OUT in template:
        form = FORM_OUT
    elif FORM_INPLACE in template:
        form = FORM_INPLACE
    else:
        return None

    # What follows the operand list is ", LUT;", so the comma goes as well as
    # the semicolon. Dropping only the semicolon left ", 0x96", which fails
    # the check below and reported every site as unreadable.
    tail = template.split(form, 1)[1]
    lut = tail.strip().lstrip(",").strip().rstrip(";").strip()
    if not lut.startswith("0x"):
        return None

    ops = operands(inner[q1 + 1:])
    outs = [o for o in ops if o[0] in ("=r", "+r")]
    ins = [o for o in ops if o[0] == "r"]
    if len(outs) != 1:
        return None

    dst = outs[0][1]
    if form == FORM_OUT and outs[0][0] == "=r" and len(ins) == 3:
        a, b, c = ins[0][1], ins[1][1], ins[2][1]
    elif form == FORM_INPLACE and outs[0][0] == "+r" and len(ins) == 2:
        a, b, c = dst, ins[0][1], ins[1][1]
    else:
        return None

    return "%s = avian_lop3(%s, %s, %s, %s);" % (dst, a, b, c, lut)


def add_header(lines):
    """Put the portable header where it is certainly in scope and legal."""
    last = -1
    for n, line in enumerate(lines[:100]):
        if line.lstrip().startswith("#include"):
            last = n
    at = last + 1 if last >= 0 else 0
    return lines[:at] + [HEADER_NOTE + "\n", HEADER + "\n"] + lines[at:]


def patch(path):
    s = io.open(path, encoding="utf-8", errors="surrogateescape",
                newline="").read()
    if "lop3.b32" not in s:
        return 0, 0
    if "avian_lop3" in s:
        return 0, s.count("lop3.b32")

    # Some of these files start with a UTF-8 BOM. Inserting the include ahead
    # of it leaves U+FEFF in the middle of a translation unit, which is not a
    # character the compiler will accept, so it is set aside and put back
    # first. Found by hexdump after simd512_func.cuh came back with the BOM
    # at byte 100.
    bom = ""
    if s.startswith("﻿"):
        bom = "﻿"
        s = s[1:]

    lines = s.split("\n")
    want = sum(1 for line in lines if "lop3.b32" in line)
    out, done = [], 0

    for line in lines:
        if "lop3.b32" not in line or line.lstrip().startswith("//"):
            out.append(line)
            continue
        body = rewrite(line)
        if body is None:
            raise SystemExit("  ERROR: %s: cannot read lop3 site:\n    %s"
                             % (os.path.basename(path), line.strip()))
        indent = line[:len(line) - len(line.lstrip())]
        out.append("#if AVIAN_NO_PTX")
        out.append(indent + body)
        out.append("#else")
        out.append(line)
        out.append("#endif")
        done += 1

    if done != want:
        raise SystemExit("  ERROR: %s: %d of %d lop3 sites rewritten"
                         % (os.path.basename(path), done, want))

    text = "\n".join(out)
    if HEADER not in text:
        text = "".join(add_header(text.splitlines(True)))
    io.open(path, "w", encoding="utf-8", errors="surrogateescape",
            newline="").write(bom + text)
    return done, want


def main():
    root = (sys.argv[1] if len(sys.argv) > 1
            else os.path.join(os.path.dirname(os.path.dirname(
                os.path.abspath(__file__))), "cuda", "upstream"))
    if not os.path.isdir(root):
        print("no such tree: %s" % root)
        return 1

    total, already, files = 0, 0, 0
    for dirpath, _dirs, names in os.walk(root):
        for name in sorted(names):
            if not name.endswith((".cu", ".cuh", ".h")):
                continue
            p = os.path.join(dirpath, name)
            done, want = patch(p)
            if done:
                print("  %s: %d lop3 site(s)" % (os.path.relpath(p, root), done))
                total += done
                files += 1
            elif want:
                already += want

    if total:
        print("  %d lop3 site(s) made portable across %d file(s)" % (total, files))
    if already:
        print("  %d site(s) already portable" % already)
    if not total and not already:
        print("  no lop3 sites found, which is itself suspicious")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

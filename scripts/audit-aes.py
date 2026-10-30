#!/usr/bin/env python3
"""Audit every cooperative shared-table fill for a barrier before first use.

The defect: these initialisers fill a shared AES table cooperatively, and
every read of the table is cross-thread, so the fill must complete for the
whole block before any thread reads. A __threadfence_block() does not do that;
only __syncthreads() does.

A 3-line lookahead is not good enough to judge this, because a correct site
may have a comment block between the call and the barrier, and a broken site
may have forty lines of constant tables before the first read. So this scans
forward from each call to the first read of the shared array and reports
whether a barrier appears in between.
"""
import os
import re
import sys

CALL = re.compile(r"\b(aes_gpu_init[A-Za-z0-9_]*|cn_aes_gpu_init[A-Za-z0-9_]*)\s*\(\s*(\w+)")
DEF = re.compile(r"^\s*(__device__|__forceinline__|void|static)")


def strip_comments(src):
    """Blank out comments, keeping every line and column so reports still
    cite the right line.

    Without this the audit reads prose about the code as if it were code.
    fix-aes-barrier.py writes "Was __threadfence_block(), which orders this
    thread's own accesses" when it installs a barrier, and that comment sits
    between the call and the __syncthreads() it just added. The scan below
    takes the first of the two primitives it sees, so it found the word in
    the comment, recorded a fence, ignored the real barrier four lines later
    and reported FENCE ONLY for the one site known to be correct.

    A check that fires on the comment describing its own fix is worse than
    no check, because the one real finding is now indistinguishable from the
    noise.
    """
    QUOTES, NL, BACKSLASH = chr(34) + chr(39), chr(10), chr(92)
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in QUOTES:
            q, j = c, i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == BACKSLASH else 1
            out.append(src[i:min(j + 1, n)])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find(NL, i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == NL else " " for ch in src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def audit(root, label):
    print("=== %s ===" % label)
    if not os.path.isdir(root):
        print("  not present at %s" % root)
        return []

    findings = []
    for dirpath, _dirs, files in os.walk(root):
        for fn in sorted(files):
            if not fn.endswith((".cu", ".cuh")):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root)
            try:
                lines = strip_comments(
                    open(path, encoding="utf-8", errors="surrogateescape").read()
                ).splitlines(keepends=True)
            except OSError:
                continue
            for i, line in enumerate(lines):
                m = CALL.search(line)
                if not m or DEF.match(line):
                    continue
                array = m.group(2)
                # Scan forward to the first read of the shared array.
                barrier = None
                first_read = None
                for j in range(i + 1, min(i + 200, len(lines))):
                    t = lines[j]
                    if "__syncthreads" in t and barrier is None:
                        barrier = j
                    if "__threadfence" in t and barrier is None:
                        barrier = -j  # negative marks a fence, not a barrier
                    # A read is the array appearing without an assignment to it.
                    if array in t and "=" not in t.split(array)[0][-3:]:
                        if re.search(re.escape(array) + r"\s*\[", t) and "] =" not in t:
                            first_read = j
                            break
                if barrier is not None and barrier > 0 and (
                        first_read is None or barrier < first_read):
                    verdict = "ok"
                elif barrier is not None and barrier < 0:
                    verdict = "FENCE ONLY"
                else:
                    verdict = "NO BARRIER"
                if verdict != "ok":
                    findings.append((rel, i + 1, m.group(1), verdict))
                print("  %-44s %-22s %s" % ("%s:%d" % (rel, i + 1), m.group(1), verdict))
    if findings:
        print("  --> %d site(s) need a barrier" % len(findings))
    else:
        print("  --> all sites synchronised")
    return findings


def main():
    """Audit one tree, given as an argument, defaulting to this checkout.

    It used to audit three hardcoded trees and ignore its argument
    entirely, so a caller passing a path got a verdict about somebody
    else's sources. That made a build-time assertion fail against the
    reference copy of upstream while the tree being built was fine.
    """
    root = (sys.argv[1] if len(sys.argv) > 1
            else os.path.join(os.path.dirname(os.path.dirname(
                os.path.abspath(__file__))), "cuda", "upstream"))
    return audit(root, root)


if __name__ == "__main__":
    sys.exit(main())

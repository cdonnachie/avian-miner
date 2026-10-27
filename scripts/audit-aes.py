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
                lines = open(path, encoding="utf-8", errors="surrogateescape").readlines()
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


if __name__ == "__main__":
    home = os.path.expanduser("~")
    scratch = ("/mnt/c/Users/craig/AppData/Local/Temp/claude/"
               "c--development-Avian-v5-0/9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635/scratchpad")
    bad = audit(home + "/avian-gpu/cpuminer-avian/cuda/upstream", "ours")
    print()
    audit(scratch + "/upstream-gpu/ccminer-tpfuemp", "tpfuemp/ccminer-tpfuemp")
    print()
    audit(home + "/avian-gpu/.suprminer-src", "suprminer-jdd, our upstream")
    sys.exit(1 if bad else 0)

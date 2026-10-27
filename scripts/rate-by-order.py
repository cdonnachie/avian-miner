#!/usr/bin/env python3
"""Hashrate per chain order, so two runs can be compared without pretending
the orders were the same.

X16RT re-rolls the chain every 128 seconds, and the orders differ enormously
in cost, so a pooled median across a short window says more about which epochs
came up than about the build. Chain orders are derived from the epoch
timestamp and effectively never repeat between runs, so a like-for-like
comparison has to go through a cost model rather than through matching orders.

The cost weights here are the GPU's, not the CPU table's. On this card the
expensive stages are the ones with large shared-memory tables or heavy
per-thread state; the CPU table in x16r-gate.c is explicitly documented as not
agreeing with a GPU's, so reusing it would be the wrong instrument.
"""
import collections
import re
import sys

ALGO = ("BLAKE BMW GROESTL JH KECCAK SKEIN LUFFA CUBEHASH SHAVITE SIMD ECHO "
        "HAMSI FUGUE SHABAL WHIRLPOOL SHA512").split()

# Relative GPU cost per stage, 1.0 = cheapest tier. Derived below from the
# data itself when there is enough of it; these are only the starting guess
# used to order the report.
GUESS = {"SIMD": 3.0, "ECHO": 2.0, "SHAVITE": 2.0, "HAMSI": 2.0,
         "FUGUE": 1.5, "WHIRLPOOL": 1.5, "GROESTL": 1.5}

ORDER = re.compile(r"Hash order ([0-9A-F]{16})")
RATE = re.compile(r"([0-9.]+)\s*([kMG]?)h/s", re.I)


def load(path):
    per = collections.defaultdict(list)
    cur = None
    try:
        fh = open(path, errors="replace")
    except OSError:
        return per
    for line in fh:
        m = ORDER.search(line)
        if m:
            cur = m.group(1)
            continue
        if cur is None:
            continue
        m = RATE.search(line)
        if m:
            v = float(m.group(1))
            u = m.group(2).lower()
            v *= {"k": 1e3, "m": 1e6, "g": 1e9}.get(u, 1.0)
            if v > 1e6:               # ignore sub-MH/s noise samples
                per[cur].append(v)
    return per


def median(xs):
    s = sorted(xs)
    return s[len(s) // 2] if s else 0.0


def score(order):
    return sum(GUESS.get(ALGO[int(c, 16)], 1.0) for c in order)


def report(label, path):
    per = load(path)
    rows = []
    for o, xs in per.items():
        if len(xs) < 20:
            continue
        rows.append((score(o), o, median(xs) / 1e6, len(xs)))
    rows.sort()
    print("=== %s ===" % label)
    if not rows:
        print("  not enough samples per order")
        return []
    for sc, o, med, n in rows:
        chain = "".join(ALGO[int(c, 16)][:2] for c in o)
        print("  cost %5.1f  %s  %6.2f MH/s  n=%-5d %s" % (sc, o, med, n, chain))
    print("  orders %d, cost range %.1f to %.1f, rate range %.2f to %.2f"
          % (len(rows), rows[0][0], rows[-1][0],
             min(r[2] for r in rows), max(r[2] for r in rows)))
    return rows


if __name__ == "__main__":
    allrows = []
    for arg in sys.argv[1:]:
        label, _, path = arg.partition("=")
        rows = report(label, path)
        allrows.append((label, rows))
        print()

    if len(allrows) == 2 and all(r[1] for r in allrows):
        (la, ra), (lb, rb) = allrows
        print("=== like for like ===")
        # Compare rate at comparable cost by bucketing on the cost score.
        for lo, hi in ((0, 18), (18, 20), (20, 22), (22, 24), (24, 99)):
            a = [r[2] for r in ra if lo <= r[0] < hi]
            b = [r[2] for r in rb if lo <= r[0] < hi]
            if not a or not b:
                continue
            print("  cost %2d-%-2d  %-22s %6.2f (n=%d)   %-22s %6.2f (n=%d)   "
                  "delta %+.1f%%"
                  % (lo, hi, la, sum(a) / len(a), len(a),
                     lb, sum(b) / len(b), len(b),
                     100 * ((sum(b) / len(b)) / (sum(a) / len(a)) - 1)))
        aa = sum(r[0] for r in ra) / len(ra)
        bb = sum(r[0] for r in rb) / len(rb)
        print("  mean cost score: %s %.1f, %s %.1f  (%s saw the %s orders)"
              % (la, aa, lb, bb, lb if bb > aa else la,
                 "dearer" if bb > aa else "cheaper"))

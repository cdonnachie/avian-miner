#!/usr/bin/env python3
"""Per-stage divergence profile, grouped by chain order and batch width.

Each batch emits one line per stage for the same chain order, so the first
stage whose count rises above zero is the one that introduces the fault. That
could not be read off separate single-stage runs, because each 20-second run
lands in a different 128-second X16RT epoch and therefore a different order.

Batch width matters too: the tail batch of a nonce range is short (768 rather
than 524288), and a fault that depends on thread index relative to a fixed
allocation would behave differently there.
"""
import collections
import re
import sys

ALGO = ("BLAKE BMW GROESTL JH KECCAK SKEIN LUFFA CUBEHASH SHAVITE SIMD ECHO "
        "HAMSI FUGUE SHABAL WHIRLPOOL SHA512").split()

LINE = re.compile(
    r"TRACE stage\s+(\d+) of ([0-9A-F]{16}):\s+(\d+)/(\d+) differ "
    r"\(stride (\d+) of (\d+)\)")
FIRST = re.compile(r"first at thread (\d+) \(nonce ([0-9a-f]{8})\)")

agg = collections.defaultdict(lambda: [0, 0])   # (order, width, stage) -> [diff, n]
threads = collections.Counter()
widths = collections.Counter()

for path in sys.argv[1:]:
    for line in open(path, errors="replace"):
        m = LINE.search(line)
        if m:
            stage, order, diff, n, _stride, width = m.groups()
            a = agg[(order, int(width), int(stage))]
            a[0] += int(diff)
            a[1] += 1
            widths[int(width)] += 1
            continue
        m = FIRST.search(line)
        if m:
            threads[int(m.group(1))] += 1

if not agg:
    print("no TRACE lines found")
    raise SystemExit(1)

for (order, width) in sorted({(o, w) for (o, w, _s) in agg}):
    chain = " ".join(ALGO[int(c, 16)][:3] for c in order)
    print("order %s  width %d" % (order, width))
    print("  chain: %s" % chain)
    rows = [(s, agg[(order, width, s)]) for s in range(1, 17)
            if (order, width, s) in agg]
    for stage, (diff, n) in rows:
        algo = ALGO[int(order[stage - 1], 16)]
        bar = "#" * int(round(diff / max(1.0, n) / 4.0))
        print("    stage %2d  %-10s %6.1f/256 over %3d batches  %s"
              % (stage, algo, diff / n, n, bar))
    nonzero = [s for s, (d, _n) in rows if d > 0]
    if nonzero:
        print("    first stage with any divergence: %d (%s)"
              % (nonzero[0], ALGO[int(order[nonzero[0] - 1], 16)]))
    else:
        print("    clean at every stage")
    print()

if threads:
    print("diverging thread indices, most common first:")
    for t, c in threads.most_common(15):
        print("  thread %-8d seen %d" % (t, c))
    print("  min %d, max %d" % (min(threads), max(threads)))

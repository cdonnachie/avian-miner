#!/usr/bin/env python3
"""Locate the second fault: chains with no SIMD that still disagree.

Attributes each candidate to the hash order in force when it was produced,
then asks, for every (algo, position) pair, how the chains containing it
behaved. A deterministic kernel fault shows up as one algo whose presence at
some position tracks the failure rate; a fault in a 64-byte kernel should be
position-independent past position 0.
"""
import re, sys, collections

ALGO = "BLAKE BMW GROESTL JH KECCAK SKEIN LUFFA CUBEHASH SHAVITE SIMD ECHO HAMSI FUGUE SHABAL WHIRLPOOL SHA512".split()

order_re = re.compile(r"Hash order ([0-9A-F]{16})")
chains = collections.Counter()   # order -> ok
bad = collections.Counter()      # order -> bad

order = None
for path in sys.argv[1:]:
    try:
        fh = open(path, errors="replace")
    except OSError:
        continue
    for line in fh:
        m = order_re.search(line)
        if m:
            order = m.group(1)
            continue
        if order is None:
            continue
        if "did not validate" in line:
            bad[order] += 1
        elif "share accepted" in line:
            chains[order] += 1

orders = sorted(set(chains) | set(bad))
tot = sum(chains.values()) + sum(bad.values())
print(f"{len(orders)} distinct orders, {tot} candidates\n")

MIN = 8
usable = [o for o in orders if chains[o] + bad[o] >= MIN]
print(f"=== per-chain, orders with >= {MIN} candidates ===")
rows = []
for o in usable:
    a, d = chains[o], bad[o]
    rows.append((100.0 * d / (a + d), o, a, d))
for r, o, a, d in sorted(rows, reverse=True):
    simd = "SIMD@%d" % o.index("9") if "9" in o else "no SIMD"
    print(f"  {o}  ok {a:<5d} bad {d:<5d} {r:5.1f}%   {simd}")

# Exclude SIMD-bearing chains: that fault is already localised.
clean = [o for o in usable if "9" not in o]
print(f"\n=== chains with no SIMD at all ({len(clean)} of {len(usable)}) ===")
if not clean:
    print("  none with enough candidates")

# Per (algo, position): failure rate pooled over chains, SIMD-free only.
print("\n=== failure rate by (algo, position), SIMD-free chains ===")
grid = collections.defaultdict(lambda: [0, 0])
for o in clean:
    for pos, ch in enumerate(o):
        g = grid[(int(ch, 16), pos)]
        g[0] += chains[o]
        g[1] += bad[o]
hits = []
for (algo, pos), (a, d) in grid.items():
    if a + d >= 20:
        hits.append((100.0 * d / (a + d), ALGO[algo], algo, pos, a, d))
for r, name, algo, pos, a, d in sorted(hits, reverse=True)[:20]:
    print(f"  {name:<10} pos {pos:<2d}  ok {a:<5d} bad {d:<5d} {r:5.1f}%")

# Marginal: presence anywhere, SIMD-free chains.
print("\n=== failure rate by algo presence (any position), SIMD-free ===")
marg = collections.defaultdict(lambda: [0, 0])
absent = collections.defaultdict(lambda: [0, 0])
for o in clean:
    present = {int(c, 16) for c in o}
    for algo in range(16):
        t = marg[algo] if algo in present else absent[algo]
        t[0] += chains[o]
        t[1] += bad[o]
for algo in range(16):
    a, d = marg[algo]
    a2, d2 = absent[algo]
    if a + d < 20 or a2 + d2 < 20:
        continue
    with_r = 100.0 * d / (a + d)
    without_r = 100.0 * d2 / (a2 + d2)
    print(f"  {ALGO[algo]:<10} with {with_r:5.1f}% (n={a+d:<5d})   "
          f"without {without_r:5.1f}% (n={a2+d2:<5d})   delta {with_r-without_r:+6.1f}")

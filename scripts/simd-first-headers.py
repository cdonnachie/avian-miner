#!/usr/bin/env python3
"""Generate headers whose X16RT chain begins with SIMD.

The defect fixed today was in x16r.c: the SIMD case hashed the uninitialised
local buffer instead of the input, which is wrong only when SIMD leads the
chain. That is one epoch in sixteen, so the six selftest vectors can easily
miss it, and did. A regression test has to pick its epochs deliberately.

The order comes from sha256d of the masked ntime, read as sixteen nibbles
from byte 7 down to byte 0, high nibble first. That is reproduced here rather
than shelled out to, so the generator does not depend on the binary under
test being correct.
"""
import hashlib
import struct
import sys

ALGO = ("BLAKE BMW GROESTL JH KECCAK SKEIN LUFFA CUBEHASH SHAVITE SIMD ECHO "
        "HAMSI FUGUE SHABAL WHIRLPOOL SHA512").split()


def order_for(ntime):
    masked = ntime & 0xFFFFFF80
    h = hashlib.sha256(hashlib.sha256(struct.pack("<I", masked)).digest()).digest()
    out = []
    for j in range(16):
        b = (15 - j) >> 1
        digit = (h[b] & 0xF) if (j & 1) else (h[b] >> 4)
        out.append("0123456789ABCDEF"[digit])
    return "".join(out)


def header_for(ntime, nonce, seed):
    """An 80-byte header in the canonical serialization the miner hashes."""
    version = struct.pack("<I", 0x20000000)
    prev = hashlib.sha256(b"avian-prev-%d" % seed).digest()
    merkle = hashlib.sha256(b"avian-merkle-%d" % seed).digest()
    return (version + prev + merkle
            + struct.pack("<I", ntime)
            + struct.pack("<I", 0x1D04FFFF)
            + struct.pack("<I", nonce))


if __name__ == "__main__":
    want = sys.argv[1] if len(sys.argv) > 1 else "simd-first"
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    base = 1760000000
    found = 0
    t = base
    while found < count and t < base + 40_000_000:
        o = order_for(t)
        hit = (o[0] == "9") if want == "simd-first" else (o[0] != "9" and "9" in o)
        if hit:
            for k in range(2):
                hdr = header_for(t, 0x10000 + k * 7919, found)
                print("%s %s %s" % (hdr.hex(), o, ALGO[int(o[0], 16)]))
            found += 1
        t += 128
    if found < count:
        print("only found %d" % found, file=sys.stderr)
        sys.exit(1)

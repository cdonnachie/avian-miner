#!/usr/bin/env python3
"""Let host code read the tables it precomputes with, under HIP.

Host code cannot read a __constant__ variable under HIP. It compiles, returns
rubbish, and reports nothing. Measured on gfx942 with a runtime index, which
is what exposes it; with a literal index the compiler folds the value out of
the initializer and the fault disappears:

    host   reads __constant__ : 00007ffffbf69ed8   <- a stack address
    host   reads plain const  : 428a2f98d728ae22
    device reads __constant__ : 428a2f98d728ae22
    device reads plain const  : 428a2f98d728ae22

sha512 precomputes nine rounds of its compression function on the host, in
setBlock_80, and reads K_512 while doing so. On AMD that read returned
4080000000000000 where the source says 428A2F98D728AE22, so every constant
handed to the kernel was wrong and x16_sha512_gpu_hash_80 differed from the
CPU in every sample. Its 64-byte kernel, which reads the same table from the
device, was exact throughout.

The qualifier is chosen per compilation pass rather than the table being
duplicated, so the two cannot drift:

    #if defined(__HIPCC__) && !defined(__HIP_DEVICE_COMPILE__)
    static const uint64_t K_512[80] =     /* host pass */
    #else
    static __constant__ const uint64_t K_512[80] =
    #endif
    { ... };

__HIP_DEVICE_COMPILE__ and not __HIPCC__, because the latter is defined in
both passes. Getting that wrong is what this script exists to avoid: the same
mistake in fix-hip-guards.py is what broke hamsi.

Only tables that host code actually reads are touched. A blanket conversion
fails: a plain const is not addressable from device code, and clang says so
with "reference to __host__ variable in __device__ function".

Idempotent, and re-applied at staging time, because cuda/upstream is vendored.
"""
import io
import os
import re
import sys

MARKER = "AVIAN_HIP_HOST_TABLE"

#: (file, table). Each is read by a __host__ function in that file, which is
#: what makes a device-only qualifier wrong for it. Listed rather than
#: detected, because reading a table from host code is rare and deliberate,
#: and a wrong guess here is silent.
SITES = [
    (os.path.join("x17", "cuda_x17_sha512.cu"), "K_512"),
]


def patch_one(s, name):
    pat = re.compile(
        r"^(?P<decl>[ \t]*(?:static\s+)?__constant__\s+[^;=\n]*?\b"
        + re.escape(name) + r"\s*\[[^\]]*\][^;=\n]*?)(?P<eq>\s*=)",
        re.MULTILINE)
    m = pat.search(s)
    if not m:
        return s, "missing"
    if MARKER in s[max(0, m.start() - 400):m.start()]:
        return s, "already"

    decl = m.group("decl")
    host = re.sub(r"__constant__\s+", "", decl, count=1)
    if not re.search(r"\bconst\b", host):
        host = host.replace(name, "const " + name, 1) if False else host
    new = ("#if defined(__HIPCC__) && !defined(__HIP_DEVICE_COMPILE__)"
           "  /* " + MARKER + " */\n"
           + host + m.group("eq") + "\n"
           "#else\n"
           + decl + m.group("eq") + "\n"
           "#endif\n")
    # The initializer's opening brace has to start a fresh line: anything
    # after #endif on the directive's own line is not code.
    rest = s[m.end():].lstrip(" \t")
    return s[:m.start()] + new + rest, "done"


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "cuda/upstream"
    done = already = 0
    for rel, name in SITES:
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            print("  not present, nothing to do: %s" % rel)
            continue
        s = io.open(p, encoding="utf-8", errors="surrogateescape",
                    newline="").read()
        s, how = patch_one(s, name)
        if how == "missing":
            raise SystemExit("%s: no __constant__ definition of %s" % (rel, name))
        if how == "done":
            io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write(s)
            print("  %s: %s is now readable from host code" % (rel, name))
            done += 1
        else:
            already += 1

    if done:
        print("  %d table(s) given a host-readable form under HIP" % done)
    elif already:
        print("  the host-read tables already have a host-readable form")
    return 0


if __name__ == "__main__":
    sys.exit(main())

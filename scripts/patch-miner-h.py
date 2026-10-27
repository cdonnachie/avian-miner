#!/usr/bin/env python3
"""Add the GPU driver's found-nonce slots to struct work."""
import io
import sys

p = sys.argv[1]
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()

anchor = "\tuint32_t data[48] __attribute__ ((aligned (64)));\n"
if s.count(anchor) != 1:
    raise SystemExit("expected 1 data[48] in struct work, found %d" % s.count(anchor))

if "uint32_t nonces[2]" in s:
    print("already present")
    raise SystemExit(0)

addition = (
    "   /* Nonces a GPU driver found in one batch, before they have been\n"
    "    * checked on the CPU. UINT32_MAX means none. ccminer's struct work\n"
    "    * carried these, and the vendored X16RT dispatch writes them in\n"
    "    * fifteen places, so they live here rather than being patched out of\n"
    "    * code that is kept verbatim on purpose. The CPU scanhash\n"
    "    * implementations do not use them. */\n"
    "   uint32_t nonces[2];\n"
)
s = s.replace(anchor, anchor + addition, 1)
io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
print("added nonces[2] to struct work")

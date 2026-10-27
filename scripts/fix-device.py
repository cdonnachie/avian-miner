#!/usr/bin/env python3
"""Stop defining cuda_arch and cuda_get_arch: cuda_checkhash.cu owns them.

Its cuda_get_arch runs a kernel to find the architecture the code was actually
compiled for, which can differ from the device's own capability once PTX is
being JIT compiled, and it treats a zero entry as "not yet asked". Setting
cuda_arch from device_sm here would make it skip that query and answer with
the device capability instead, so the whole thing is left alone.
"""
import io
import sys

p = sys.argv[1]
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()

cuts = [
    "int   cuda_arch[MAX_GPUS] = { 0 };\n",
    """
int cuda_get_arch(int thr_id)
{
    return cuda_arch[device_map[thr_id]];
}
""",
    "        cuda_arch[i] = (int) device_sm[i];\n",
]

changed = False
for c in cuts:
    if c in s:
        s = s.replace(c, "", 1)
        changed = True

note = """/* cuda_arch and cuda_get_arch are defined by the vendored cuda_checkhash.cu,
 * which asks the device which architecture its code was built for rather than
 * reading the capability. Deliberately not defined here. */
"""
if changed and note not in s:
    anchor = "short device_mpcount[MAX_GPUS] = { 0 };\n"
    if s.count(anchor) == 1:
        s = s.replace(anchor, anchor + "\n" + note, 1)

if not changed:
    print("already fixed")
else:
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("removed cuda_arch and cuda_get_arch from device.cu")

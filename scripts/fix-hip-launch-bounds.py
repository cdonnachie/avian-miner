#!/usr/bin/env python3
"""Let HIP take the same launch-bounds branch the host already chose.

The third consequence of hipcc never defining __CUDA_ARCH__, and the first
that waits until run time to show itself. The two sides of a kernel disagree:

    host    tpb = TPB52;                           // 512
            if (device_sm[dev_id] <= 500) tpb = TPB50;

    device  #if __CUDA_ARCH__ > 500
            __launch_bounds__(TPB52, 3)            // 512
            #else
            __launch_bounds__(TPB50, 5)            // 256
            #endif

device_sm is computed from the device properties, and on gfx942 those report
major.minor 9.4, so device_sm is 940 and the host picks 512. The device-side
guard is false under hipcc whatever the hardware, so the kernel is compiled
for 256. Launching 512 threads at it fails:

    Launch params (512, 1, 1) are larger than launch bounds (256)
      for kernel quark_skein512_gpu_hash_64
    GPU #0: the hash chain left "unspecified launch failure"

Widening the guard is the fix rather than lowering the host's block size,
because the host is not confused: 940 really is a modern, large GPU, and 512
is well inside this card's 1024 thread limit. The macro is what is wrong, not
the decision it was standing in for.

Only guards that immediately precede a __launch_bounds__ are touched. Other
uses of __CUDA_ARCH__ select algorithm variants, and the portable fallbacks
behind them are deliberately the ones HIP takes; widening those would undo
most of the port.

Idempotent, and re-applied at staging time, because cuda/upstream is vendored
and a re-vendor would drop this with no build failing to say so. It would,
however, fail at run time, which is how this was found.
"""
import io
import os
import re
import sys

GUARD = re.compile(r"^(\s*#if\s+__CUDA_ARCH__\s*>\s*\d+)\s*$")
ADDITION = " || defined(__HIPCC__)"
#: How far ahead of a __launch_bounds__ the guard may sit. Two lines covers
#: both spellings in the tree: the attribute alone on the next line, and
#: `__global__ __launch_bounds__(...)` on it.
LOOKAHEAD = 2


def patch_file(p):
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    lines = s.splitlines(True)
    out = []
    n = 0
    for i, line in enumerate(lines):
        m = GUARD.match(line.rstrip("\r\n"))
        if m and "__HIPCC__" not in line:
            ahead = "".join(lines[i + 1:i + 1 + LOOKAHEAD])
            if "__launch_bounds__" in ahead:
                eol = line[len(line.rstrip("\r\n")):]
                out.append(m.group(1) + ADDITION + eol)
                n += 1
                continue
        out.append(line)
    if n:
        io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                newline="").write("".join(out))
    return n


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "cuda/upstream"
    if not os.path.isdir(root):
        print("  not present, nothing to do: %s" % root)
        return 0

    total = 0
    for dirpath, _dirs, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith((".cu", ".cuh")):
                continue
            p = os.path.join(dirpath, f)
            n = patch_file(p)
            if n:
                print("  %s: %d guard(s) widened" % (os.path.relpath(p, root), n))
                total += n

    if total:
        print("  %d launch-bounds guard(s) now include HIP" % total)
    else:
        print("  every launch-bounds guard already includes HIP")
    return 0


if __name__ == "__main__":
    sys.exit(main())

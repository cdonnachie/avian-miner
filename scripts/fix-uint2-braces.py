#!/usr/bin/env python3
"""Brace the elements of uint2 array initializers written as a flat list.

The only genuine type-system difference the HIP port turned up. CUDA's uint2
is a plain struct, so brace elision applies and an array of them can be
initialised from a flat list of twice as many scalars:

    const uint2 __align__(16) cmsg[16] = {
        0xaaaaaaa0, 0xaaaaaaaa, 0xaaaaaaa1, 0xaaaaaaaa, ...

HIP's uint2 is HIP_vector_type, a class template, so elision does not apply
and each element has to be initialised in its own right:

    error: no viable conversion from 'unsigned int' to 'const uint2'
           (aka 'const HIP_vector_type<unsigned int, 2>')

Pairing the scalars is correct for both: `{ x, y }` is aggregate
initialisation for the struct and a two-argument constructor call for the
class, so no AVIAN_NO_PTX switch is needed and the CUDA build sees the same
values it saw before.

Nothing else in the tree needs this. cuda_bmw512.cu writes precalc[16] four
lines below cmsg[16] in the braced form already, and so does every other
uint2 table, which is both why there are so few of these and why the braced
form is what this script emits: it is the file's own house style.

Idempotent, since a braced body no longer matches, and re-applied at staging
time, because cuda/upstream is vendored and a re-vendor would drop this with
no build failing to say so.
"""
import io
import os
import re
import sys

# A uint2 array initializer whose body contains no brace of its own. The
# element count is captured so the scalar count can be checked against it
# rather than assumed.
INIT = re.compile(
    r"(\buint2\b[^;=\n]*\[\s*(\d+)\s*\]\s*=\s*\{\n)([^{}]*?)(\n[ \t]*\})")

PER_LINE = 4


def rewrite(body, count, name):
    """Pair a flat scalar list into braced elements, keeping the indent."""
    indent = re.match(r"[ \t]*", body).group(0)
    # Comments inside these tables would be dropped by the rebuild below, so
    # a body carrying one is refused rather than quietly rewritten.
    if "//" in body or "/*" in body:
        raise SystemExit("%s: initializer carries a comment; not rewriting it"
                         % name)

    items = [t.strip() for t in body.split(",")]
    if items and items[-1] == "":
        items.pop()  # a trailing comma is allowed
    if any(t == "" for t in items):
        raise SystemExit("%s: empty element in the initializer" % name)
    if len(items) == count:
        # One expression per element, each already of uint2 type, as in
        # `uint2 v[16] = { h[0], ..., z[7] };`. No elision is happening, so
        # HIP accepts it as it stands and there is nothing to pair.
        return None
    if len(items) != 2 * count:
        raise SystemExit(
            "%s: %d scalars for %d uint2 elements; this is not a flat list of "
            "pairs and pairing it would silently change the values"
            % (name, len(items), count))

    pairs = ["{ %s, %s }" % (items[i], items[i + 1])
             for i in range(0, len(items), 2)]
    lines = []
    for i in range(0, len(pairs), PER_LINE):
        lines.append(indent + ", ".join(pairs[i:i + PER_LINE]))
    return ",\n".join(lines)


def patch_file(p):
    s = io.open(p, encoding="utf-8", errors="surrogateescape",
                newline="").read()
    out = []
    pos = 0
    n = 0
    for m in INIT.finditer(s):
        head, count, body, tail = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        name = "%s:%d" % (os.path.basename(p), s.count("\n", 0, m.start()) + 1)
        paired = rewrite(body, count, name)
        if paired is None:
            continue
        out.append(s[pos:m.start()])
        out.append(head + paired + tail)
        pos = m.end()
        n += 1
    if n:
        out.append(s[pos:])
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
            if not f.endswith((".cu", ".cuh", ".h")):
                continue
            p = os.path.join(dirpath, f)
            n = patch_file(p)
            if n:
                rel = os.path.relpath(p, root)
                print("  %s: %d initializer(s) braced" % (rel, n))
                total += n

    if total:
        print("  %d flat uint2 initializer(s) now brace their elements" % total)
    else:
        print("  every uint2 initializer already braces its elements")
    return 0


if __name__ == "__main__":
    sys.exit(main())

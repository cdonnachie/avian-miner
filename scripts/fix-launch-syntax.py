#!/usr/bin/env python3
"""Normalise spaced kernel-launch syntax to <<<...>>>.

Upstream writes launches as

    x17_sha512_gpu_hash_64 << <grid, block >> > (threads, d_hash);

nvcc accepts the spaces. clang in HIP mode does not, and reports

    error: expected expression

which is a confusing thing to be told about a line that is plainly a launch.
Measured with HIP 7.2, 74 sites across 29 files, every one on a single line,
and they were the largest remaining group of errors after the lop3 work.

This changes nothing for CUDA: <<< and << < are the same launch to nvcc, so
the normalised form compiles identically. It is applied to the vendored tree
rather than hand-edited because cuda/upstream is vendored, and re-applied at
staging time for the same reason as the other patches here.

Deliberately narrow. The transformation only fires on a line that contains
both an opening << < and a closing >> >, because neither sequence is valid
C++ in any other context: `a << <` and `a >> >` do not parse. A launch
spanning two lines would be left alone and reported, rather than guessed at.
"""
import io
import os
import re
import sys

# The whole launch on one line: the chevrons, whatever is between them, and
# the closing pair. The body is matched non-greedily so a line with two
# launches is handled one at a time.
LAUNCH = re.compile(r"<<\s+<(.*?)>>\s*>|<<\s*<(.*?)>>\s+>")

EXTS = (".cu", ".cuh", ".h")


def fix(line):
    """Return (new_line, how_many_launches_changed)."""
    n = 0

    def sub(m):
        nonlocal n
        n += 1
        body = m.group(1) if m.group(1) is not None else m.group(2)
        return "<<<" + body + ">>>"

    return LAUNCH.sub(sub, line), n


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    changed_files = 0
    changed_sites = 0
    straddling = []

    for dirpath, _dirs, names in os.walk(root):
        for name in sorted(names):
            if not name.endswith(EXTS):
                continue
            p = os.path.join(dirpath, name)
            s = io.open(p, encoding="utf-8", errors="surrogateescape",
                        newline="").read()
            if "<< <" not in s and "<<  <" not in s:
                continue

            out = []
            n_here = 0
            for i, line in enumerate(s.split("\n"), 1):
                if re.search(r"<<\s+<", line) and not re.search(r">>\s*>", line):
                    # An opening with no closing on the same line. Not
                    # transformed, because the body is then unknown to this
                    # script and a wrong guess would be a silent miscompile.
                    straddling.append("%s:%d" % (p, i))
                    out.append(line)
                    continue
                new, n = fix(line)
                n_here += n
                out.append(new)

            if n_here:
                io.open(p, "w", encoding="utf-8", errors="surrogateescape",
                        newline="").write("\n".join(out))
                changed_files += 1
                changed_sites += n_here

    for loc in straddling:
        print("  LAUNCH SPANS LINES, left alone: %s" % loc)

    if changed_sites:
        print("  normalised %d launch site(s) in %d file(s)"
              % (changed_sites, changed_files))
    else:
        print("  every launch already uses <<<...>>>")

    # A straddling launch is not an error here, but it does mean the tree is
    # not fully HIP-ready and nothing else would say so.
    return 1 if straddling else 0


if __name__ == "__main__":
    sys.exit(main())

#!/bin/bash
# Collect cpuminer.exe and every non-system DLL it needs, transitively.
#
# The first level is short, but MSYS2's libcurl drags in OpenSSL, zstd,
# brotli, nghttp2, idn2 and more. Walking only the direct imports produces a
# zip that works on the build machine and fails everywhere else, which is the
# classic way to ship a broken Windows build.
set -u
B=${B:-/c/Users/craig/AppData/Local/Temp/claude/c--development-Avian-v5-0/9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635/scratchpad/winbuild/cpuminer-avian}
REL=${REL:-/c/Users/craig/AppData/Local/Temp/claude/c--development-Avian-v5-0/9e1a4f58-d2d0-4ac5-b59e-ba1e8ee54635/scratchpad/winbuild/release}

rm -rf "$REL"; mkdir -p "$REL"
cp "$B/avian-miner.exe" "$REL/" || exit 1

# Breadth-first over the import tables. Anything present in /mingw64/bin is
# ours to ship; anything else is a Windows system DLL and must not be copied.
declare -A seen
queue=("$REL/avian-miner.exe")
while [ ${#queue[@]} -gt 0 ]; do
    cur="${queue[0]}"; queue=("${queue[@]:1}")
    for d in $(objdump -p "$cur" 2>/dev/null | awk '/DLL Name:/ {print $3}'); do
        key=$(echo "$d" | tr 'A-Z' 'a-z')
        [ -n "${seen[$key]:-}" ] && continue
        seen[$key]=1
        if [ -f "/mingw64/bin/$d" ]; then
            cp -n "/mingw64/bin/$d" "$REL/"
            queue+=("$REL/$d")
        fi
    done
done

echo "=== packaged ==="
ls -1 "$REL" | sed 's/^/  /'
echo
echo "  $(ls -1 "$REL" | wc -l) files, $(du -sh "$REL" | cut -f1)"

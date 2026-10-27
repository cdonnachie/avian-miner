#!/usr/bin/env python3
"""Add --gpu, which swaps X16RT's CPU scan for the CUDA one."""
import io
import os
import sys

root = sys.argv[1]


def patch(rel, pairs, skip_if=None):
    p = os.path.join(root, rel)
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    if skip_if and skip_if in s:
        print("  %s already patched" % rel)
        return
    for old, new in pairs:
        if s.count(old) != 1:
            raise SystemExit("%s: expected 1 of %r, found %d" % (rel, old[:60], s.count(old)))
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("  patched", rel)


# ------------------------------------------------------------------- miner.h
patch("miner.h", [
    # The long option. Listed unguarded like the sv2 ones, so that a build
    # without CUDA still recognises it and can say why it will not work.
    ('        { "avian-selftest", 0, NULL, 1206 },\n',
     '        { "avian-selftest", 0, NULL, 1206 },\n'
     '        { "gpu", 0, NULL, 1207 },\n'),
    ("      --avian-selftest          check -a's proof of work against hashes a real node reported\\n\\",
     "      --avian-selftest          check -a's proof of work against hashes a real node reported\\n\\\n"
     "      --gpu                     mine x16rt on the GPU instead of the CPU, one thread per card\\n\\"),
    ("extern bool opt_debug_diff;",
     "extern bool opt_debug_diff;\n"
     "/* --gpu. Read by register_x16rt_algo, which hands the gate to the CUDA\n"
     " * backend instead of a CPU scanhash. */\n"
     "extern bool opt_gpu;"),
], skip_if='{ "gpu", 0, NULL, 1207 }')

# -------------------------------------------------------------- cpu-miner.c
patch("cpu-miner.c", [
    ("enum algos opt_algo = ALGO_NULL;",
     "enum algos opt_algo = ALGO_NULL;\n"
     "bool opt_gpu = false;"),
    # Placed after the Stratum v2 block's #endif, because --gpu has nothing to
    # do with Stratum v2 and should work in a build without it.
    ("   case 1206:  // avian-selftest\n      opt_avian_selftest = true;\n      break;\n#endif\n",
     "   case 1206:  // avian-selftest\n      opt_avian_selftest = true;\n      break;\n#endif\n"
     "   case 1207:  // gpu\n"
     "#if defined(HAVE_CUDA)\n"
     "      opt_gpu = true;\n"
     "#else\n"
     "      applog( LOG_ERR, \"this build has no GPU backend; configure with \"\n"
     "                       \"--with-cuda to get one\" );\n"
     "      show_usage_and_exit(1);\n"
     "#endif\n"
     "      break;\n"),
], skip_if="case 1207:")

# ------------------------------------------------------------- x16r-gate.c
patch("algo/x16/x16r-gate.c", [
    ("""bool register_x16rt_algo( algo_gate_t* gate )
{
""",
     """bool register_x16rt_algo( algo_gate_t* gate )
{
#if defined(HAVE_CUDA)
  /* --gpu replaces the CPU scan rather than adding to it. One algorithm runs
   * per process here, so a GPU run is wholly a GPU run, and the CPU is left
   * for the MinotaurX process alongside it. */
  if ( opt_gpu )
     return register_x16rt_cuda_algo( gate );
#endif
"""),
], skip_if="register_x16rt_cuda_algo")

#!/usr/bin/env python3
"""Add --sv2-hashrate, so the miner can tell the pool how fast it is.

nominal_hash_rate was hardcoded to 1000.0f. The pool sizes its share rate
limiter from it: allowed = max(claimed * probability * tolerance, floor), so a
claim of 1 kH/s lands on the 2/s floor no matter how fast the miner really is,
and everything beyond that comes back as too-many-shares. The field is a claim
in Stratum v2, which is why it is an option rather than something measured:
the channel opens before any hashing has happened.
"""
import io
import os
import sys

root = sys.argv[1]


def patch(rel, pairs, skip_if):
    p = os.path.join(root, rel)
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    if skip_if in s:
        print("  %s already patched" % rel)
        return
    for old, new in pairs:
        if s.count(old) != 1:
            raise SystemExit("%s: expected 1 of %r, found %d" % (rel, old[:60], s.count(old)))
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("  patched", rel)


patch("miner.h", [
    ('        { "gpu", 0, NULL, 1207 },\n',
     '        { "gpu", 0, NULL, 1207 },\n'
     '        { "sv2-hashrate", 1, NULL, 1208 },\n'),
    ("      --gpu                     mine x16rt on the GPU instead of the CPU, one thread per card\\n\\",
     "      --gpu                     mine x16rt on the GPU instead of the CPU, one thread per card\\n\\\n"
     "      --sv2-hashrate=RATE       tell the pool how fast this miner is, eg 30M. The pool sizes\\n\\\n"
     "                                its share rate limit from this, so leaving it low gets shares\\n\\\n"
     "                                refused as too-many-shares\\n\\"),
    ("extern bool opt_gpu;",
     "extern bool opt_gpu;\n"
     "/* --sv2-hashrate, in hashes per second. What the miner claims when it\n"
     " * opens a channel; the pool sizes its rate limiter from it. */\n"
     "extern double opt_sv2_hashrate;"),
], skip_if='"sv2-hashrate"')

patch("cpu-miner.c", [
    ("bool opt_gpu = false;",
     "bool opt_gpu = false;\n"
     "/* Hashes per second. The old hardcoded 1000.0 put every miner on the\n"
     " * pool's 2/s share floor regardless of what it could actually do. */\n"
     "double opt_sv2_hashrate = 1000.0;"),
    ("   case 1207:  // gpu\n",
     "   case 1208:  // sv2-hashrate\n"
     "   {\n"
     "      char *end = NULL;\n"
     "      double v = strtod( arg, &end );\n"
     "      /* k, M and G, because nobody wants to type 30000000. */\n"
     "      if ( end && *end )\n"
     "      {\n"
     "         if ( *end == 'k' || *end == 'K' )      v *= 1e3;\n"
     "         else if ( *end == 'm' || *end == 'M' ) v *= 1e6;\n"
     "         else if ( *end == 'g' || *end == 'G' ) v *= 1e9;\n"
     "         else v = 0.0;\n"
     "      }\n"
     "      if ( !( v > 0.0 ) )\n"
     "      {\n"
     "         applog( LOG_ERR, \"--sv2-hashrate wants a positive rate, \"\n"
     "                          \"optionally suffixed k, M or G: %s\", arg );\n"
     "         show_usage_and_exit(1);\n"
     "      }\n"
     "      opt_sv2_hashrate = v;\n"
     "      break;\n"
     "   }\n"
     "   case 1207:  // gpu\n"),
], skip_if="case 1208:")

patch("avian/sv2.c", [
    ("    config.nominal_hash_rate = 1000.0f;",
     "    /* What the pool sizes its share rate limiter from, so a miner that\n"
     "     * understates it has its shares refused as too-many-shares however\n"
     "     * valid they are. Stratum v2 calls this nominal because it is a\n"
     "     * claim: the channel opens before any hashing has happened, so\n"
     "     * there is nothing measured to report yet. */\n"
     "    config.nominal_hash_rate = (float) opt_sv2_hashrate;"),
], skip_if="opt_sv2_hashrate")

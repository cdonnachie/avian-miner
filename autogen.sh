#!/bin/sh

# You need autoconf 2.5x, preferably 2.57 or later
# You need automake 1.7 or later. 1.6 might work.

set -e

# -f throughout, because autoconf and autoheader otherwise skip the work when
# the output is not older than its input. The committed configure and
# cpuminer-config.h.in are deliberately stale, and a fresh git clone writes
# every file in the same instant, so whether they got regenerated came down to
# the order checkout happened to write two files in. When it went the wrong
# way, configure ran without ever having heard of --with-cuda and reported
# "unrecognized options" among its usual noise.
aclocal
autoheader -f
automake --foreign --add-missing --force-missing --copy
# automake --gnu --add-missing --copy
autoconf -f


#!/usr/bin/env python3
"""Go back to upstream's per-algorithm floor, because measurement says so.

Scaling throughput from free device memory looked like the obvious fix for a
table of device names that stops at Turing. Measured on an RTX 3090, 45s per
point, mining a real pool:

    throughput   accepted   disagreements   MH/s
    524288 (1<<19)   77          0          30.5
    1048576          79          2          29.7
    2097152          26         17          25.1
    4194304          33         19          25.6

Bigger batches are both slower and wrong. The disagreements are candidates the
GPU reports whose hash, recomputed on the CPU, is nowhere near target, and they
appear from 1<<21 up, which is the signature of a buffer sized for one batch
and used with another: upstream hands x11_simd512_cpu_init throughput>>4 under
splitsimd, and that is the obvious suspect.

So the floor stands, not from caution this time but from evidence. The env
override stays for whoever picks the splitsimd thread up.
"""
import io
import sys

p = sys.argv[1]
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()

start = s.index("/* Provisional ceiling.")
end = s.index("double throughput2intensity(uint32_t throughput)")

new = r'''uint32_t cuda_default_throughput(int thr_id, uint32_t defthr)
{
    const char *env;

    /* Upstream picks defthr from a chain of strstr() over the device name
     * that stops at Turing, so an Ampere or later card lands on its 1<<19
     * floor. That looks like an oversight worth correcting by scaling from
     * device memory, and it is not: measured on a 3090 against a live pool,
     * 45 seconds a point,
     *
     *     1<<19   30.5 MH/s    0 bad candidates
     *     1<<20   29.7 MH/s    2
     *     1<<21   25.1 MH/s   17
     *     1<<22   25.6 MH/s   19
     *
     * bigger batches are slower *and* start producing candidates whose hash
     * does not survive being recomputed on the CPU. The second column is the
     * reason this is not merely a tuning preference. Those appear from 1<<21,
     * which is what a buffer sized for one batch and used with another looks
     * like; upstream gives x11_simd512_cpu_init throughput>>4 under
     * splitsimd, which is the place to start.
     *
     * So the floor stands on evidence rather than on caution. Raising it
     * means fixing that first, and AVIAN_CUDA_THROUGHPUT is how to
     * experiment without a rebuild.
     */
    env = getenv("AVIAN_CUDA_THROUGHPUT");
    if (env && *env)
    {
        unsigned long v = strtoul(env, NULL, 0);
        if (v >= 0x100)
        {
            applog(LOG_INFO, "GPU #%d: throughput %lu, from AVIAN_CUDA_THROUGHPUT",
                   (int) device_map[thr_id], v);
            return (uint32_t) (v & 0xFFFFFF00);
        }
        applog(LOG_WARNING, "ignoring AVIAN_CUDA_THROUGHPUT=%s, below the 256 minimum", env);
    }

    return defthr;
}

'''

io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s[:start] + new + s[end:])
print("throughput back to upstream's floor, with the measurement recorded")

#!/usr/bin/env python3
"""Scale throughput from free device memory instead of upstream's name table.

Upstream picked a default from a chain of strstr() over the device name that
stops at Turing, so a 3090 lands on its 1<<19 floor and leaves most of the card
idle. That floor was kept until the memory cost was known; it is measurable
now, so this scales from what the device reports free.
"""
import io
import sys

p = sys.argv[1]
s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()

start = s.index("uint32_t cuda_default_throughput(int thr_id, uint32_t defthr)")
end = s.index("double throughput2intensity(uint32_t throughput)")

new = r'''/* Provisional ceiling. d_hash alone takes 64 bytes a nonce, and the per-kernel
 * init allocations take more again, by an amount nobody has measured. Capping
 * well below what the memory arithmetic allows keeps the first GPU builds on
 * the safe side of that ignorance: being slower than the card allows is
 * recoverable, running out of device memory part way through a scan is not.
 * Raise it once the scratch cost has actually been measured. */
#define CUDA_MAX_THROUGHPUT (1u << 22)

uint32_t cuda_default_throughput(int thr_id, uint32_t defthr)
{
    size_t freemem = 0, total = 0;
    uint64_t thr;
    const char *env;

    /* A testing escape hatch until there is a real --cuda-intensity. Takes a
     * nonce count, not an intensity, so it can be set to anything rather than
     * only to powers of two. */
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

    cudaSetDevice(device_map[thr_id]);
    if (cudaMemGetInfo(&freemem, &total) != cudaSuccess || freemem == 0)
    {
        /* Without a figure to work from, upstream's floor is the honest
         * answer rather than a guess. */
        return defthr;
    }

    /* An eighth of what is free, at 64 bytes a nonce. The other seven eighths
     * cover the per-kernel scratch and whatever else shares the card: on a
     * desktop that is the compositor, which on this machine already holds
     * several gigabytes. */
    thr = (uint64_t) (freemem / 8) / 64;
    if (thr < defthr) thr = defthr;
    if (thr > CUDA_MAX_THROUGHPUT) thr = CUDA_MAX_THROUGHPUT;

    applog(LOG_INFO, "GPU #%d: %lu MB free, throughput %u",
           (int) device_map[thr_id], (unsigned long) (freemem >> 20),
           (uint32_t) (thr & 0xFFFFFF00));

    return (uint32_t) (thr & 0xFFFFFF00);
}

'''

s = s[:start] + new + s[end:]
if "#include <stdlib.h>" not in s:
    s = s.replace("#include <string.h>", "#include <stdlib.h>\n#include <string.h>", 1)
io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
print("throughput now scales from free device memory, capped at 1<<22")

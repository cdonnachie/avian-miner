/* CUDA device inventory, and the host globals the vendored kernels expect.
 *
 * ccminer kept these in cuda.cpp alongside a lot of machinery this miner has
 * no use for: fan and temperature monitoring, an API server, an algorithm
 * benchmark sweep. Only the parts the X16RT path reads are here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cuda/avian-cuda-win-compat.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include "avian-cuda-shim.h"
/* The authority on device_map, device_sm, device_mpcount and cuda_arch. */
#include "cuda_helper.h"
#include "avian-cuda-device.h"

/* Index is the mining thread, value the CUDA device ordinal, so a future
 * --gpu-devices can reorder or restrict without the kernels knowing. */
short device_map[MAX_GPUS] = { 0 };
long  device_sm[MAX_GPUS] = { 0 };
short device_mpcount[MAX_GPUS] = { 0 };

/* cuda_arch and cuda_get_arch are defined by the vendored cuda_checkhash.cu,
 * which asks the device which architecture its code was built for rather than
 * reading the capability. Deliberately not defined here. */

char   device_name[MAX_GPUS][64] = { { 0 } };
size_t device_mem[MAX_GPUS] = { 0 };

int cuda_num_devices = 0;
int opt_cudaschedule = -1;
int gpu_threads = 1;

/* See the shim: this miner has no algorithm sweep, so the vendored hash check
 * must never take its early return. */
int bench_algo = -1;

cudaError_t MyStreamSynchronize(cudaStream_t stream, int situation,
                                           int thr_id)
{
    /* Upstream varied this with --cuda-schedule and a per-situation table.
     * Synchronising unconditionally is the conservative reading and costs
     * only the chance to overlap work the X16RT chain does not overlap
     * anyway, since each of its sixteen stages consumes the previous one. */
    (void) situation;
    (void) thr_id;
    return cudaStreamSynchronize(stream);
}

void cudaReportHardwareFailure(int thr_id, cudaError_t err,
                                          const char *func)
{
    applog(LOG_ERR, "GPU #%d: %s: %s", (int) device_map[thr_id], func,
           cudaGetErrorString(err));
    /* Upstream slept here so a card failing every launch does not spin the
     * log. Keeping that, because the alternative is unbounded output. */
    sleep(1);
}

int cuda_detect_devices(void)
{
    int n = 0;
    cudaError_t err = cudaGetDeviceCount(&n);

    if (err != cudaSuccess)
    {
        /* A missing driver and a present one with no supported card report
         * differently, and this message is the only clue the user gets. */
        applog(LOG_ERR, "CUDA: %s", cudaGetErrorString(err));
        cuda_num_devices = 0;
        return 0;
    }
    if (n > MAX_GPUS)
    {
        applog(LOG_WARNING, "CUDA: %d devices found, using the first %d", n,
               MAX_GPUS);
        n = MAX_GPUS;
    }

    for (int i = 0; i < n; i++)
    {
        cudaDeviceProp props;

        device_map[i] = (short) i;
        if (cudaGetDeviceProperties(&props, i) != cudaSuccess)
        {
            snprintf(device_name[i], sizeof(device_name[i]), "unknown");
            continue;
        }
        snprintf(device_name[i], sizeof(device_name[i]), "%s", props.name);
        device_sm[i] = props.major * 100 + props.minor * 10;
        device_mpcount[i] = (short) props.multiProcessorCount;
        device_mem[i] = props.totalGlobalMem;
    }

    cuda_num_devices = n;
    return n;
}

void cuda_print_devices(void)
{
    for (int i = 0; i < cuda_num_devices; i++)
    {
        const int d = device_map[i];

        applog(LOG_INFO, "GPU #%d: %s, SM %ld.%ld, %d MP, %lu MB", d,
               device_name[d], device_sm[d] / 100, (device_sm[d] / 10) % 10,
               (int) device_mpcount[d], (unsigned long) (device_mem[d] >> 20));
    }
}

uint32_t cuda_default_throughput(int thr_id, uint32_t defthr)
{
    const char *env;

    /* Upstream picks defthr from a chain of strstr() over the device name
     * that stops at Turing, so an Ampere or later card lands on whatever the
     * driver gives an unlisted one. Upstream's figure for that is 1<<19, and
     * the driver now passes 1<<21 on Nvidia. Measured on a 3090 with
     * --gpu --benchmark, which pins the chain to FE17C8172501B98C, each
     * figure the rate over thirty seconds of batches once the card is warm:
     *
     *     1<<18   37.6 MH/s
     *     1<<19   40.5 MH/s
     *     1<<20   41.9 MH/s
     *     1<<21   42.6 MH/s
     *     1<<22   42.7 MH/s
     *
     * with no candidate failing its CPU check at any size. The time is not
     * in the gap between batches, which is 0.2 ms whatever the size; it is
     * in the launches, about fifty of them a batch once SIMD is split
     * sixteen ways, each with a start and a tail in which the card is not
     * full. 1<<21 is where that stops paying, at 49 ms a batch, and it
     * costs about 550 MB of device memory with SIMD split. Running SIMD
     * unsplit gets 3% of it back at 1<<19 and nothing at 1<<21, for 2 GB.
     *
     * A table that stood here said the opposite: slower from 1<<20 and
     * producing candidates the CPU rejected from 1<<21. It was taken
     * against a live pool at 45 seconds a point, so each point sat in a
     * different 128-second epoch and the rate column compared chain orders
     * rather than batch sizes. And it predates 28a0015 and e6ce169, either
     * of which accounts for the rejected candidates: a verifier that hashed
     * an uninitialised buffer when SIMD led the chain, and a share target
     * read once per scan while the pool tightened it. At 1<<21 the device
     * now agrees with the CPU at every stage of seventeen chain orders,
     * which between them put every function first and in the middle.
     *
     * AVIAN_CUDA_THROUGHPUT is how to sweep another card without a rebuild.
     */
    env = getenv("AVIAN_CUDA_THROUGHPUT");
    if (env && *env)
    {
        /* Said once a device. The driver asks for this at the top of every
         * batch, so saying it each time was a line a batch: fifty a second
         * on a 3070 at 1<<19, which scrolls the Benchmark line the variable
         * is there to produce off the screen. */
        static bool said[MAX_GPUS] = { false };
        const bool first = thr_id >= 0 && thr_id < MAX_GPUS && !said[thr_id];
        unsigned long v = strtoul(env, NULL, 0);

        if (first)
            said[thr_id] = true;
        if (v >= 0x100)
        {
            if (first)
                applog(LOG_INFO, "GPU #%d: throughput %lu, from AVIAN_CUDA_THROUGHPUT",
                       (int) device_map[thr_id], v);
            return (uint32_t) (v & 0xFFFFFF00);
        }
        if (first)
            applog(LOG_WARNING, "ignoring AVIAN_CUDA_THROUGHPUT=%s, below the 256 minimum", env);
    }

    return defthr;
}

double throughput2intensity(uint32_t throughput)
{
    double intensity = 0.0;
    uint32_t ws = throughput;
    uint8_t i = 0;

    while (ws > 1 && i++ < 32)
        ws = ws >> 1;
    intensity = (double) i;
    if (i && throughput > (1u << i))
        intensity += ((double) (throughput - (1u << i)) / (1u << i));

    return intensity;
}

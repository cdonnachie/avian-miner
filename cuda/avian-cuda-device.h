#ifndef AVIAN_CUDA_DEVICE_H
#define AVIAN_CUDA_DEVICE_H 1

#include <stdint.h>
#include <stddef.h>

#include "avian-cuda-shim.h"

/* device_map, device_sm, device_mpcount and cuda_arch are declared by the
 * vendored cuda_helper.h, which is the authority on their types: device_sm is
 * long and device_mpcount is short, which is easy to get wrong. They are
 * deliberately not repeated here. Only names ccminer kept elsewhere, in
 * cuda.cpp or its option parsing, are below.
 */

#ifdef __cplusplus
extern "C" {
#endif

extern char device_name[MAX_GPUS][64];
extern size_t device_mem[MAX_GPUS];

/* Number of usable devices, set by cuda_detect_devices. Zero means no GPU
 * algorithm can run, which belongs at startup rather than inside a scan. */
extern int cuda_num_devices;

/* cudaDeviceScheduleBlockingSync and friends, or -1 to leave the choice to
 * the driver. Upstream exposed this as --cuda-schedule. */
extern int opt_cudaschedule;

/* Mining threads per device. One, unless a reason appears to run more. The
 * vendored driver reads it to decide whether resetting the device at startup
 * is safe. */
extern int gpu_threads;

int  cuda_detect_devices(void);
void cuda_print_devices(void);

/* Nonces per kernel launch for this thread's device. defthr is the
 * algorithm's own floor. */
uint32_t cuda_default_throughput(int thr_id, uint32_t defthr);

/* Throughput as ccminer's -i expressed it, for logging. */
double throughput2intensity(uint32_t throughput);

#ifdef __cplusplus
}
#endif

#endif /* AVIAN_CUDA_DEVICE_H */

/* The DLL's own support code: the one bridge left, plus the exported entry
 * points that are not the batch function itself.
 *
 * Measured, the CUDA objects needed twenty miner-side symbols. Moving
 * verification and submission to the miner removed nineteen of them. The one
 * that remains is applog, because the kernels have plenty to say and no way
 * to say it: the miner's logger lives on the other side of the boundary. So
 * the miner hands one in, and until it does the DLL stays silent rather than
 * writing to a stdout nobody is reading.
 *
 * Compiled into the DLL only. On Linux the miner's own applog is linked
 * directly and this file is not built.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <cuda_runtime.h>

#include "cuda/avian-cuda-api.h"

static avian_cuda_log_fn g_log = NULL;

#ifdef AVIAN_CUDA_BUILDING_DLL
/* Several vendored kernels read opt_debug directly to gate extra logging.
 * It stays 0 in the DLL rather than being wired to the miner's copy: it
 * controls nothing but log verbosity inside the kernels, and plumbing a
 * debug flag across the ABI would be a Windows-only divergence for no
 * behavioural difference. The batch function uses job->debug where it
 * matters. */
extern "C" int opt_debug = 0;
#endif

extern "C" AVIAN_CUDA_API void avian_cuda_set_logger(avian_cuda_log_fn fn)
{
    g_log = fn;
}

#ifdef AVIAN_CUDA_BUILDING_DLL
/* Matches the declaration the kernels see in avian-cuda-shim.h. Variadic and
 * formatted here, so the miner receives a finished line and does not have to
 * reproduce printf semantics across the boundary.
 *
 * Only in the DLL. Linked into the miner directly, as it is on Linux, the
 * kernels reach the miner's own applog and defining a second one here would
 * be a duplicate symbol. g_log then goes unused, which is harmless and
 * keeps the gate's startup identical on both platforms. */
extern "C" void applog(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    if (g_log == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    g_log(level, buf);
}
#endif /* AVIAN_CUDA_BUILDING_DLL */

extern "C" AVIAN_CUDA_API int avian_cuda_device_count(void)
{
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) {
        return -1;
    }
    return n;
}

extern "C" AVIAN_CUDA_API const char *avian_cuda_device_name(int dev_id)
{
    /* Static per call site rather than touching the miner's device_name
     * table, which is the kernels' own and indexed by thread, not device. */
    static char name[256];
    cudaDeviceProp prop;

    if (dev_id < 0 || cudaGetDeviceProperties(&prop, dev_id) != cudaSuccess) {
        return "";
    }
    strncpy(name, prop.name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    return name;
}

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
#include "cuda/avian-cuda-device.h"

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

/* Takes the device inventory, not just a count.
 *
 * cuda_detect_devices fills device_map, device_name, device_sm and
 * device_mpcount, which the vendored kernels read to pick a device and to
 * size their launches. Returning a bare cudaGetDeviceCount instead left all
 * of that zeroed, and every candidate the device reported then disagreed
 * with the CPU: 153 disagreements and not one accepted share. The inventory
 * belongs on this side of the boundary, because device.cu is in the DLL and
 * the miner cannot reach it. */
extern "C" AVIAN_CUDA_API int avian_cuda_device_count(void)
{
    static int done = 0;

    if (!done) {
        cuda_detect_devices();
        done = 1;
    }
    return cuda_num_devices;
}

extern "C" AVIAN_CUDA_API const char *avian_cuda_device_name(int dev_id)
{
    /* From the inventory, so the name matches the device the kernels will
     * actually use. Calling this before avian_cuda_device_count would read
     * an unfilled table, so take the inventory first; it is guarded and
     * only runs once. */
    (void) avian_cuda_device_count();
    if (dev_id < 0 || dev_id >= cuda_num_devices) {
        return "";
    }
    return device_name[dev_id];
}

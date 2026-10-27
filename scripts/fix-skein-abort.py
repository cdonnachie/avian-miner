#!/usr/bin/env python3
"""Stop skein512_cpu_setBlock_80 aborting the miner on an error it did not
cause.

Run after re-vendoring the CUDA kernels. Upstream does not have this.

What happens
------------
Almost nothing in the vendored kernels reads a CUDA error. The one exception
is skein512_cpu_setBlock_80, which ends with

    cudaMemcpyToSymbol(c_buffer, buffer, sizeof(c_buffer), 0, ...);
    CUDA_SAFE_CALL(cudaGetLastError());

cudaGetLastError() returns the last error from *any* preceding call in the
thread, and CUDA_SAFE_CALL calls exit(). So this one line kills the miner
over whatever happens to be pending, and it only runs when SKEIN leads the
chain: one epoch in sixteen, which is roughly every half hour.

What was measured
-----------------
With the call instrumented to report its own return value separately from
the sticky error:

    SKEIN as the first batch   inherited=no error        copy=no error
    SKEIN after any other      inherited=invalid argument  copy=no error

So the copy succeeds. The symbol resolves (cudaGetSymbolAddress returns a
valid address). The sizes match: a 1024-byte host buffer sourcing 960 bytes
into a 960-byte __constant__. Candidates from the affected batches verify
correctly against the CPU, and the miner mines at 100% acceptance and has
found a mainnet block. The error is real but it is not this call's, and it
does not affect results.

What was not found
------------------
Where the error comes from. Checks were placed at batch entry, after the
target upload, after device setup, after every individual chain stage with a
cudaDeviceSynchronize between them, and at batch end capturing the
synchronise's own return. All report success, and SKEIN still inherits an
error with no CUDA call between the last clean check and its own. That
should not be possible in one thread, so something about the model is still
wrong. Removing cudaDeviceReset() from the init path did not change it, and
neither did CUDA_MODULE_LOADING=EAGER.

Why downgrade rather than fix
-----------------------------
Aborting the process is the wrong response to an error this call did not
produce, whatever its origin. The error is reported once at warning level so
it stays visible rather than being swallowed. The underlying error remains
open; this only stops it taking the miner down.
"""
import io
import os
import sys

OLD = ("\tcudaMemcpyToSymbol(c_buffer, buffer, sizeof(c_buffer), 0, "
       "cudaMemcpyHostToDevice);\n"
       "\tCUDA_SAFE_CALL(cudaGetLastError());\n")

NEW = """\t{
\t\t/* Not CUDA_SAFE_CALL(cudaGetLastError()). That reads the last
\t\t * error from any preceding call and then exits the process, so
\t\t * this one line killed the miner over errors it did not cause,
\t\t * one epoch in sixteen. Measured: the copy below succeeds and
\t\t * the symbol resolves; only the inherited error differs between
\t\t * a run where SKEIN leads first and one where it does not. See
\t\t * scripts/fix-skein-abort.py. */
\t\tcudaError_t rc = cudaMemcpyToSymbol(c_buffer, buffer,
\t\t                                    sizeof(c_buffer), 0,
\t\t                                    cudaMemcpyHostToDevice);
\t\tif (rc != cudaSuccess)
\t\t\tfprintf(stderr, "skein512_cpu_setBlock_80: %s\\n",
\t\t\t        cudaGetErrorString(rc));
\t}
"""


def main():
    root = (sys.argv[1] if len(sys.argv) > 1
            else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    p = os.path.join(root, "cuda", "upstream", "quark", "cuda_skein512.cu")
    try:
        s = io.open(p, encoding="utf-8", errors="surrogateescape",
                    newline="").read()
    except OSError as e:
        print("  %s" % e)
        return 1

    if "fix-skein-abort.py" in s:
        print("  already patched")
        return 0
    if s.count(OLD) != 1:
        print("  ERROR: call site matched %d times" % s.count(OLD))
        return 1

    io.open(p, "w", encoding="utf-8", errors="surrogateescape",
            newline="").write(s.replace(OLD, NEW, 1))
    print("  skein512_cpu_setBlock_80 no longer exits on an inherited error")
    return 0


if __name__ == "__main__":
    sys.exit(main())

/* Plain-C forms of the primitives the vendored kernels write as inline PTX.
 *
 * PTX is Nvidia's, so every asm block in cuda/upstream is a wall between
 * these kernels and any other vendor. There are about 39 such expressions in
 * live code, 60 of them lop3.b32 alone once the helper headers are counted,
 * and a HIP or OpenCL backend has to answer every one.
 *
 * The answer is not "hand-write 60 boolean expressions and hope", which is a
 * transcription job with 60 chances to be subtly wrong in a hash. It is to
 * decode each one from the immediate it already carries. See avian_lop3.
 *
 * AVIAN_NO_PTX selects the portable path. It is set automatically under
 * hipcc, and can be forced on nvcc with -DAVIAN_NO_PTX=1, which is the
 * point: the portable path is then verifiable on the hardware we have. Build
 * with it, run the forced-order stage bisect, and 0 differing across every
 * stage of a chain is direct evidence that the plain-C forms are
 * hash-identical to the PTX they replace. Without that, a port to AMD would
 * rest on a reading of the PTX ISA rather than on a measurement.
 *
 * Keeping both paths rather than deleting the PTX is deliberate. The asm is
 * there because it is faster, and on Nvidia it stays the default.
 */
#ifndef AVIAN_PTX_PORTABLE_H
#define AVIAN_PTX_PORTABLE_H

#include <stdint.h>

#if !defined(AVIAN_NO_PTX)
#  if defined(__HIP_PLATFORM_AMD__) || defined(__HIPCC__)
/* HIP has no PTX, so there is nothing to choose. */
#    define AVIAN_NO_PTX 1
#  else
#    define AVIAN_NO_PTX 0
#  endif
#endif

/* nvcc has __device__ and __forceinline__ built in. HIP does not: they come
 * from hip/hip_runtime.h, so without this the header compiles only when
 * something else happened to include that first. Measured with HIP 7.2:
 * __forceinline__ is undefined before that include and defined after.
 */
#if defined(__HIPCC__)
#  include <hip/hip_runtime.h>
#endif

#if defined(__CUDACC__) || defined(__HIPCC__)
#  define AVIAN_PTX_FN static __device__ __forceinline__
#else
#  define AVIAN_PTX_FN static inline
#endif

/* lop3.b32 evaluated from its own truth table.
 *
 * PTX defines the immediate as a truth table over three inputs, indexed by
 * (a_bit << 2) | (b_bit << 1) | c_bit. The vendored sources confirm it
 * themselves: cuda_helper.h annotates its most common immediate as
 * "0x96 = 0xF0 ^ 0xCC ^ 0xAA", and 0xF0, 0xCC and 0xAA are precisely the
 * tables that select a, b and c, so 0x96 is their three-way xor.
 *
 * Writing the decode once makes all 60 call sites correct by construction
 * instead of correct by careful reading. lut is a compile-time constant at
 * every site, so the compiler folds the tests away and is left with the
 * boolean expression it would have been given by hand; on AMD it can fold
 * much of it into v_bfi_b32 or v_perm_b32.
 */
AVIAN_PTX_FN uint32_t avian_lop3(uint32_t a, uint32_t b, uint32_t c,
                                 unsigned lut)
{
        uint32_t r = 0;

        if (lut & 0x01u) r |= ~a & ~b & ~c;
        if (lut & 0x02u) r |= ~a & ~b &  c;
        if (lut & 0x04u) r |= ~a &  b & ~c;
        if (lut & 0x08u) r |= ~a &  b &  c;
        if (lut & 0x10u) r |=  a & ~b & ~c;
        if (lut & 0x20u) r |=  a & ~b &  c;
        if (lut & 0x40u) r |=  a &  b & ~c;
        if (lut & 0x80u) r |=  a &  b &  c;
        return r;
}

#endif /* AVIAN_PTX_PORTABLE_H */

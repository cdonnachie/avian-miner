#include "cuda_helper.h"

/* Macros for uint2 operations (used by skein) */


static __device__ __forceinline__ uint2 operator+ (const uint2 a, const uint32_t b)
{
#if 0 && defined(__CUDA_ARCH__) && CUDA_VERSION < 7000
	uint2 result;
	asm(
		"add.cc.u32 %0,%2,%4; \n\t"
		"addc.u32 %1,%3,%5;   \n\t"
	: "=r"(result.x), "=r"(result.y) : "r"(a.x), "r"(a.y), "r"(b), "r"(0));
	return result;
#else
	return vectorize(devectorize(a) + b);
#endif
}
#if defined(__HIPCC__)
/* AVIAN_HIP_UINT2_SCALAR: an int literal is not an exact match for the uint32_t
 * overload above, and HIP's uint2 can be constructed from a scalar, so
 * without this `x + 8` builds a uint2(8, 8) and operates on each half.
 * Forwards rather than reimplements, so the two cannot disagree. See
 * scripts/fix-hip-uint2-scalar.py. */
static __device__ __forceinline__
uint2 operator+ (const uint2 a, const int b)
{
	return a + (uint32_t)b;
}
#endif


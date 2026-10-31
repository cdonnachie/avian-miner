/* What every avian-miner OpenCL round kernel needs, beyond the sph preamble.
 *
 * Separate from sph-common.cl so that file stays as it came from sgminer,
 * which makes it reviewable against upstream.
 */

#ifndef AVIAN_OCL_COMMON_CL
#define AVIAN_OCL_COMMON_CL 1

/* One work item's 64 bytes. The three views are not a convenience: the
 * algorithms disagree about word size, and whirlpool and shavite address the
 * same bytes as 32-bit words where jh and skein want 64-bit ones.
 *
 * The same union sgminer's bitblock.cl declares, which is why its per-stage
 * kernels port into this with their bodies untouched.
 */
typedef union {
    uchar h1[64];
    uint  h4[16];
    ulong h8[8];
} hash_t;

/* SHL, SHR and CONST_EXP2 are bmw.cl's, but sgminer defines them in the chain
 * file rather than in bmw.cl, so a kernel that includes only bmw.cl does not
 * get them. Copied from bitblock.cl, which is MIT; see sph-common.cl for the
 * notice. CONST_EXP2 is a line-for-line copy because it is an expression
 * macro, not a definition that could be shortened.
 */
#define SHL(x, n) ((x) << (n))
#define SHR(x, n) ((x) >> (n))

/* DEC32LE and DEC64LE read a little-endian word out of the 80-byte header.
 *
 * sph-common.cl comes from darkcoin.cl, whose chain starts with blake and so
 * only ever needed the big-endian readers. The chain files for coins starting
 * with something else define these alongside them, and the first rounds taken
 * from those files use them. Copied from fresh.cl and whirlcoin.cl, which are
 * MIT; see sph-common.cl for the notice.
 */
#if SPH_BIG_ENDIAN
  #define DEC32LE(x) SWAP4(*(const __global sph_u32 *) (x))
  #define DEC64LE(x) SWAP8(*(const __global sph_u64 *) (x))
#else
  #define DEC32LE(x) (*(const __global sph_u32 *) (x))
  #define DEC64LE(x) (*(const __global sph_u64 *) (x))
#endif

#define CONST_EXP2  q[i+0] + SPH_ROTL64(q[i+1], 5)  + q[i+2] + SPH_ROTL64(q[i+3], 11) + \
                    q[i+4] + SPH_ROTL64(q[i+5], 27) + q[i+6] + SPH_ROTL64(q[i+7], 32) + \
                    q[i+8] + SPH_ROTL64(q[i+9], 37) + q[i+10] + SPH_ROTL64(q[i+11], 43) + \
                    q[i+12] + SPH_ROTL64(q[i+13], 53) + (SHR(q[i+14],1) ^ q[i+14]) + (SHR(q[i+15],2) ^ q[i+15])

#endif /* AVIAN_OCL_COMMON_CL */

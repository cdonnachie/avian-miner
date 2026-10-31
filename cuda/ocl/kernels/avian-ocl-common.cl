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
  #define DEC32BE(x) (*(const __global sph_u32 *) (x))
#else
  #define DEC32LE(x) (*(const __global sph_u32 *) (x))
  #define DEC64LE(x) (*(const __global sph_u64 *) (x))
  #define DEC32BE(x) SWAP4(*(const __global sph_u32 *) (x))
#endif

#define CONST_EXP2  q[i+0] + SPH_ROTL64(q[i+1], 5)  + q[i+2] + SPH_ROTL64(q[i+3], 11) + \
                    q[i+4] + SPH_ROTL64(q[i+5], 27) + q[i+6] + SPH_ROTL64(q[i+7], 32) + \
                    q[i+8] + SPH_ROTL64(q[i+9], 37) + q[i+10] + SPH_ROTL64(q[i+11], 43) + \
                    q[i+12] + SPH_ROTL64(q[i+13], 53) + (SHR(q[i+14],1) ^ q[i+14]) + (SHR(q[i+15],2) ^ q[i+15])

#endif /* AVIAN_OCL_COMMON_CL */

/* Where the nonce goes, and in which orientation.
 *
 * Every -80 kernel injects the nonce itself, by whatever mechanism its own
 * state uses, and they all put it at header bytes 76 to 79. What they share
 * is the orientation: those four bytes hold the nonce big-endian.
 *
 * That follows from the header. job.data is "the header in byte-swapped
 * words" per cuda/avian-cuda-api.h, and avian-ocl.c swaps each word before
 * upload, so word 19's bytes are be32enc of the nonce, which is exactly what
 * algo/x16/x16rt-cuda-host.c writes into edata[19] before re-hashing a
 * candidate on the CPU.
 *
 * This backend used to upload job.data unswapped and write the nonce
 * little-endian. The two were consistent with each other, so every kernel
 * agreed with a CPU chain run over the same bytes and avian-ocl-batchtest
 * was happy, but those were not the eighty bytes the miner hashes. The
 * device found genuine candidates, at the right rate, for the wrong message,
 * and the miner rejected all of them: a backend that mined nothing while
 * reporting a hashrate. avian-ocl-batchtest now verifies the way the miner
 * verifies, and covers all sixteen first-round kernels rather than three.
 */

/* BLAKE-512, one X16RT round: 64 bytes in place, per work item.
 *
 * Adapted rather than ported. Every sgminer chain file has blake first, where
 * it hashes the 80-byte header, so none of them contains a 64-byte blake
 * round. The compression itself is upstream's: COMPRESS64 out of blake.cl,
 * which is MIT. Only the message block and the counter differ, and those come
 * from our own cuda/upstream/quark/cuda_quark_blake512.cu, whose
 * quark_blake512_gpu_hash_64 is the same round on the same input size.
 *
 * The padding, which is the part worth being explicit about. A 64-byte
 * message occupies words 0 to 7 of a 128-byte block, so:
 *
 *   M8   the 0x80 terminator
 *   MD   the 1 that BLAKE's padding sets before the length
 *   MF   the length in bits, 512
 *   T0   the counter, also 512
 *
 * The 80-byte stage puts its terminator at MA and its length at MF = 0x280,
 * because ten words of it are message. Getting either of those wrong gives a
 * hash that looks plausible and is not, which is what avian-ocl-selftest is
 * for.
 *
 * sgminer writes T0 as 0xFFFFFFFFFFFFFC00 + (80 << 3) and then adds 1024 with
 * a carry into T1, which arrives at 640 by a route that only makes sense when
 * a block loop is doing it. There is no loop here, so it is written as the
 * value it is.
 *
 * avian-ocl-entry: blake512_64
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "blake.cl"

__kernel void blake512_64(__global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv = hashes[gid];
  hash_t *hash = &avian_priv;

  sph_u64 H0 = SPH_C64(0x6A09E667F3BCC908), H1 = SPH_C64(0xBB67AE8584CAA73B);
  sph_u64 H2 = SPH_C64(0x3C6EF372FE94F82B), H3 = SPH_C64(0xA54FF53A5F1D36F1);
  sph_u64 H4 = SPH_C64(0x510E527FADE682D1), H5 = SPH_C64(0x9B05688C2B3E6C1F);
  sph_u64 H6 = SPH_C64(0x1F83D9ABFB41BD6B), H7 = SPH_C64(0x5BE0CD19137E2179);
  sph_u64 S0 = 0, S1 = 0, S2 = 0, S3 = 0;
  sph_u64 T0 = 512, T1 = 0;

  sph_u64 M0, M1, M2, M3, M4, M5, M6, M7;
  sph_u64 M8, M9, MA, MB, MC, MD, ME, MF;
  sph_u64 V0, V1, V2, V3, V4, V5, V6, V7;
  sph_u64 V8, V9, VA, VB, VC, VD, VE, VF;

  M0 = SWAP8(hash->h8[0]);
  M1 = SWAP8(hash->h8[1]);
  M2 = SWAP8(hash->h8[2]);
  M3 = SWAP8(hash->h8[3]);
  M4 = SWAP8(hash->h8[4]);
  M5 = SWAP8(hash->h8[5]);
  M6 = SWAP8(hash->h8[6]);
  M7 = SWAP8(hash->h8[7]);
  M8 = 0x8000000000000000UL;
  M9 = 0;
  MA = 0;
  MB = 0;
  MC = 0;
  MD = 1;
  ME = 0;
  MF = 0x200;

  COMPRESS64;

  hash->h8[0] = SWAP8(H0);
  hash->h8[1] = SWAP8(H1);
  hash->h8[2] = SWAP8(H2);
  hash->h8[3] = SWAP8(H3);
  hash->h8[4] = SWAP8(H4);
  hash->h8[5] = SWAP8(H5);
  hash->h8[6] = SWAP8(H6);
  hash->h8[7] = SWAP8(H7);

  hashes[gid] = avian_priv;
}

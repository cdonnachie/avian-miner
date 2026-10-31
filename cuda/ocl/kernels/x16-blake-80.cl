/* BLAKE-512, the X16RT first round: the 80-byte header in, 64 bytes out.
 *
 * sgminer's `search` from bitblock.cl, which is MIT; see blake.cl for the
 * notice. That kernel is the only 80-byte stage any of the chain files has,
 * because every one of them puts blake first, so this is the one first round
 * that ports rather than being adapted.
 *
 * Three changes from upstream. The nonce comes from nonce_base + gid rather
 * than gid alone, because the backend scans a range and the host decides
 * where it starts. The counter is written as its value, 640 bits for 80
 * bytes, rather than reached by upstream's add-with-carry, which only makes
 * sense when a block loop is driving it. And the digest is byte-swapped on
 * the way out, which is what makes it agree with sph_blake512 rather than
 * with whatever the next stage of sgminer's chain happened to want.
 *
 * The nonce lands at bytes 76 to 79 of the header. M9 holds bytes 72 to 79
 * read big-endian, so masking off its low half and xoring in SWAP4 of the
 * nonce writes the nonce's little-endian bytes there, which is where the
 * miner's own header has them.
 *
 * avian-ocl-entry: blake512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "blake.cl"

__kernel void blake512_80(__global const uchar* block, const uint nonce_base,
                          __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv;
  hash_t *hash = &avian_priv;

  sph_u64 H0 = SPH_C64(0x6A09E667F3BCC908), H1 = SPH_C64(0xBB67AE8584CAA73B);
  sph_u64 H2 = SPH_C64(0x3C6EF372FE94F82B), H3 = SPH_C64(0xA54FF53A5F1D36F1);
  sph_u64 H4 = SPH_C64(0x510E527FADE682D1), H5 = SPH_C64(0x9B05688C2B3E6C1F);
  sph_u64 H6 = SPH_C64(0x1F83D9ABFB41BD6B), H7 = SPH_C64(0x5BE0CD19137E2179);
  sph_u64 S0 = 0, S1 = 0, S2 = 0, S3 = 0;
  sph_u64 T0 = 640, T1 = 0;

  sph_u64 M0, M1, M2, M3, M4, M5, M6, M7;
  sph_u64 M8, M9, MA, MB, MC, MD, ME, MF;
  sph_u64 V0, V1, V2, V3, V4, V5, V6, V7;
  sph_u64 V8, V9, VA, VB, VC, VD, VE, VF;

  M0 = DEC64BE(block +  0);
  M1 = DEC64BE(block +  8);
  M2 = DEC64BE(block + 16);
  M3 = DEC64BE(block + 24);
  M4 = DEC64BE(block + 32);
  M5 = DEC64BE(block + 40);
  M6 = DEC64BE(block + 48);
  M7 = DEC64BE(block + 56);
  M8 = DEC64BE(block + 64);
  M9 = DEC64BE(block + 72);
  M9 &= 0xFFFFFFFF00000000UL;
  M9 ^= SWAP4(nonce_base + gid);
  MA = 0x8000000000000000UL;
  MB = 0;
  MC = 0;
  MD = 1;
  ME = 0;
  MF = 0x280;

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

/* keccak, the X16RT first round: the 80-byte header in, 64 bytes out.
 *
 * sgminer's search5 from bitblock.cl, which is MIT; see keccak.cl for the
 * notice it belongs to. bitblock.cl is x15 written as one kernel per stage
 * over a global hash_t array, which is this backend's architecture too, so
 * the body below is upstream's with four changes and no more: the kernel is
 * renamed, sgminer's reqd_work_group_size attribute is dropped, the global
 * offset term is dropped, and SWAP8/SWAP4 are stripped where they wrap a
 * hash-> load or store.
 *
 * That last one is the only interesting change. The chain holds its
 * inter-stage hash in byte-swapped words where our sph does not, so dropping
 * the swap at the boundary is what makes this byte-exact against sph. It is
 * stripped only against hash->h8[] and hash->h4[], never generally, because
 * several of these algorithms use the same macros internally and a blanket
 * strip would leave a kernel that compiles and hashes wrongly.
 * doc/building.md records the comparison that established the rule, and
 * avian-ocl-selftest is what holds it to account per algorithm.
 *
 * avian-ocl-entry: keccak512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "keccak.cl"

__kernel void keccak512_80(__global const uchar* block, const uint nonce_base,
                           __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv;
  hash_t *hash = &avian_priv;

  // keccak

  sph_u64 a00 = 0, a01 = 0, a02 = 0, a03 = 0, a04 = 0;
  sph_u64 a10 = 0, a11 = 0, a12 = 0, a13 = 0, a14 = 0;
  sph_u64 a20 = 0, a21 = 0, a22 = 0, a23 = 0, a24 = 0;
  sph_u64 a30 = 0, a31 = 0, a32 = 0, a33 = 0, a34 = 0;
  sph_u64 a40 = 0, a41 = 0, a42 = 0, a43 = 0, a44 = 0;

  a10 = SPH_C64(0xFFFFFFFFFFFFFFFF);
  a20 = SPH_C64(0xFFFFFFFFFFFFFFFF);
  a31 = SPH_C64(0xFFFFFFFFFFFFFFFF);
  a22 = SPH_C64(0xFFFFFFFFFFFFFFFF);
  a23 = SPH_C64(0xFFFFFFFFFFFFFFFF);
  a04 = SPH_C64(0xFFFFFFFFFFFFFFFF);

  /* Keccak-512's rate is 72 bytes, nine words, so 80 bytes is two blocks
   * where 64 is one. The 64-byte round puts 0x8000000000000001 in rate
   * word 8, which is both halves of Keccak's padding at once: 0x01 at
   * block byte 64 and the final bit at byte 71, the two landing in the
   * same word. Split across two blocks they separate.
   *
   * Absorption is XOR, which commutes with the lane complement, so the
   * complemented lanes need no special handling here and the pair
   * un-complemented at the end is unchanged. */

  /* block 1: nine words of header, 72 bytes */
  a00 ^= ((__global const ulong *) block)[0];
  a10 ^= ((__global const ulong *) block)[1];
  a20 ^= ((__global const ulong *) block)[2];
  a30 ^= ((__global const ulong *) block)[3];
  a40 ^= ((__global const ulong *) block)[4];
  a01 ^= ((__global const ulong *) block)[5];
  a11 ^= ((__global const ulong *) block)[6];
  a21 ^= ((__global const ulong *) block)[7];
  a31 ^= ((__global const ulong *) block)[8];
  KECCAK_F_1600;

  /* block 2: the last header word, then the padding. The nonce is header
   * bytes 76 to 79, the high half of word 9. */
  a00 ^= (((__global const ulong *) block)[9] & 0x00000000FFFFFFFFUL)
         | (((ulong) SWAP4(nonce_base + gid)) << 32);
  a10 ^= 0x01;                      /* at block byte 8, past the word */
  a31 ^= 0x8000000000000000UL;      /* the final bit, at byte 71 */
  KECCAK_F_1600;

  // Finalize the "lane complement"
  a10 = ~a10;
  a20 = ~a20;

  hash->h8[0] = a00;
  hash->h8[1] = a10;
  hash->h8[2] = a20;
  hash->h8[3] = a30;
  hash->h8[4] = a40;
  hash->h8[5] = a01;
  hash->h8[6] = a11;
  hash->h8[7] = a21;

  hashes[gid] = avian_priv;
}

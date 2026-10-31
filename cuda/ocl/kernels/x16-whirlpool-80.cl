/* whirlpool, the X16RT first round: the 80-byte header in, 64 bytes out.
 *
 * sgminer's search14 from bitblock.cl, which is MIT; see whirlpool.cl for the
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
 * avian-ocl-entry: whirlpool512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "whirlpool.cl"

__kernel void whirlpool512_80(__global const uchar* block, const uint nonce_base,
                              __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  #define HD(i) (((__global const ulong *) block)[i])
  hash_t avian_priv;
  hash_t *hash = &avian_priv;

  __local sph_u64 LT0[256], LT1[256], LT2[256], LT3[256], LT4[256], LT5[256], LT6[256], LT7[256];

  int init = get_local_id(0);
  int step = get_local_size(0);

  for (int i = init; i < 256; i += step)
  {
    LT0[i] = plain_T0[i];
    LT1[i] = plain_T1[i];
    LT2[i] = plain_T2[i];
    LT3[i] = plain_T3[i];
    LT4[i] = plain_T4[i];
    LT5[i] = plain_T5[i];
    LT6[i] = plain_T6[i];
    LT7[i] = plain_T7[i];
  }

  barrier(CLK_LOCAL_MEM_FENCE);
  /* Every table read below goes through LT, not plain_T. The staging
   * above was here already and three of the four round macros still
   * read __constant, so 16 KB was copied into local memory each launch
   * and then mostly ignored. A constant cache broadcasts one address to
   * a whole warp; these indices are bytes of the state, so the lanes
   * diverge and the read serialises. That made whirlpool about 82%% of
   * an X16RT batch once groestl was fixed the same way. plain_RC stays
   * in __constant deliberately: it is indexed by the round counter, so
   * every lane does read the same address. */


  // whirlpool
  sph_u64 n0, n1, n2, n3, n4, n5, n6, n7;
  sph_u64 h0, h1, h2, h3, h4, h5, h6, h7;
  sph_u64 state[8];

  n0 = HD(0);
  n1 = HD(1);
  n2 = HD(2);
  n3 = HD(3);
  n4 = HD(4);
  n5 = HD(5);
  n6 = HD(6);
  n7 = HD(7);

  h0 = h1 = h2 = h3 = h4 = h5 = h6 = h7 = 0;

  n0 ^= h0;
  n1 ^= h1;
  n2 ^= h2;
  n3 ^= h3;
  n4 ^= h4;
  n5 ^= h5;
  n6 ^= h6;
  n7 ^= h7;

  #pragma unroll 10
  for (unsigned r = 0; r < 10; r ++)
  {
    sph_u64 tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;

    ROUND_KSCHED(LT, h, tmp, plain_RC[r]);
    TRANSFER(h, tmp);
    ROUND_WENC(LT, n, h, tmp);
    TRANSFER(n, tmp);
  }

  state[0] = n0 ^ HD(0);
  state[1] = n1 ^ HD(1);
  state[2] = n2 ^ HD(2);
  state[3] = n3 ^ HD(3);
  state[4] = n4 ^ HD(4);
  state[5] = n5 ^ HD(5);
  state[6] = n6 ^ HD(6);
  state[7] = n7 ^ HD(7);

  /* Whirlpool takes 64-byte blocks, so 80 bytes is one message block then a
   * final block holding two header words, the terminator and the length.
   * The nonce is header bytes 76 to 79, the high half of word 9.
   *
   * 0x8002000000000000 is 640 bits big-endian in the block's last eight
   * bytes read back as a little-endian word, the same encoding jh uses,
   * where the 64-byte round has 512 as 0x2000000000000. */
  n0 = HD(8);
  n1 = (HD(9) & 0x00000000FFFFFFFFUL)
       | (((ulong) SWAP4(nonce_base + gid)) << 32);
  n2 = 0x80;
  n3 = n4 = n5 = n6 = 0;
  n7 = 0x8002000000000000UL;

  h0 = state[0];
  h1 = state[1];
  h2 = state[2];
  h3 = state[3];
  h4 = state[4];
  h5 = state[5];
  h6 = state[6];
  h7 = state[7];

  n0 ^= h0;
  n1 ^= h1;
  n2 ^= h2;
  n3 ^= h3;
  n4 ^= h4;
  n5 ^= h5;
  n6 ^= h6;
  n7 ^= h7;

  #pragma unroll 10
  for (unsigned r = 0; r < 10; r ++)
  {
    sph_u64 tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;

    ROUND_KSCHED(LT, h, tmp, plain_RC[r]);
    TRANSFER(h, tmp);
    ROUND_WENC(LT, n, h, tmp);
    TRANSFER(n, tmp);
  }

  state[0] ^= n0 ^ HD(8);
  state[1] ^= n1 ^ ((HD(9) & 0x00000000FFFFFFFFUL)
                    | (((ulong) SWAP4(nonce_base + gid)) << 32));
  state[2] ^= n2 ^ 0x80;
  state[3] ^= n3;
  state[4] ^= n4;
  state[5] ^= n5;
  state[6] ^= n6;
  state[7] ^= n7 ^ 0x8002000000000000UL;

  for (unsigned i = 0; i < 8; i ++)
    hash->h8[i] = state[i];


  barrier(CLK_GLOBAL_MEM_FENCE);
  hashes[gid] = avian_priv;
  #undef HD
}

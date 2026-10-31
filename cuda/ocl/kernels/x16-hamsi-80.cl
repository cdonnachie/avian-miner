/* hamsi, the X16RT first round: the 80-byte header in, 64 bytes out.
 *
 * sgminer's search11 from bitblock.cl, which is MIT; see hamsi.cl for the
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
 * avian-ocl-entry: hamsi512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"

/* Set from outside the algorithm's own header, as sgminer's chain
 * files do; see port-bitblock.sh. */
#define SPH_HAMSI_EXPAND_BIG 1

#include "hamsi.cl"

__kernel void hamsi512_80(__global const uchar* block, const uint nonce_base,
                          __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv;
  hash_t *hash = &avian_priv;
  const uint avian_nonce = nonce_base + gid;

  #ifdef INPUT_BIG_LOCAL
    __local sph_u32 T512_L[1024];
    __constant const sph_u32 *T512_C = &T512[0][0];

    int init = get_local_id(0);
    int step = get_local_size(0);
    for (int i = init; i < 1024; i += step)
      T512_L[i] = T512_C[i];

    barrier(CLK_LOCAL_MEM_FENCE);
  #else
    #define INPUT_BIG_LOCAL INPUT_BIG
  #endif

  sph_u32 c0 = HAMSI_IV512[0], c1 = HAMSI_IV512[1], c2 = HAMSI_IV512[2], c3 = HAMSI_IV512[3];
  sph_u32 c4 = HAMSI_IV512[4], c5 = HAMSI_IV512[5], c6 = HAMSI_IV512[6], c7 = HAMSI_IV512[7];
  sph_u32 c8 = HAMSI_IV512[8], c9 = HAMSI_IV512[9], cA = HAMSI_IV512[10], cB = HAMSI_IV512[11];
  sph_u32 cC = HAMSI_IV512[12], cD = HAMSI_IV512[13], cE = HAMSI_IV512[14], cF = HAMSI_IV512[15];
  sph_u32 m0, m1, m2, m3, m4, m5, m6, m7;
  sph_u32 m8, m9, mA, mB, mC, mD, mE, mF;
  sph_u32 h[16] = { c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, cA, cB, cC, cD, cE, cF };

  /* Bytewise, so there is no word order to get wrong: bytes 76 to 79 are
   * the nonce little-endian, exactly as the miner's header holds it, and
   * everything else comes straight from the header. */
  #define buf(u) (((i + u) >= 76) ? ((uchar) (avian_nonce >> (8 * ((i + u) - 76)))) : block[i + u])

  for(int i = 0; i < 80; i += 8)
  {
    INPUT_BIG_LOCAL;
    P_BIG;
    T_BIG;
  }

  #undef buf
  #define buf(u) (u == 0 ? 0x80 : 0)

  INPUT_BIG_LOCAL;
  P_BIG;
  T_BIG;

  #undef buf
  /* The message length, big-endian in the last two bytes of the block:
   * 640 bits is 0x0280, where the 64-byte round has 0x0200. */
  #define buf(u) (u == 6 ? 2 : (u == 7 ? 0x80 : 0))

  INPUT_BIG_LOCAL;
  PF_BIG;
  T_BIG;

  for (unsigned u = 0; u < 16; u ++)
      hash->h4[u] = h[u];


    for (int i = 0; i < 16; i++) hash->h4[i] = SWAP4(hash->h4[i]);
  hashes[gid] = avian_priv;
  #undef buf
}

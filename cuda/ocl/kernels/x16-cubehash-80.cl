/* cubehash, the X16RT first round: the 80-byte header in, 64 bytes out.
 *
 * sgminer's search7 from bitblock.cl, which is MIT; see cubehash.cl for the
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
 * avian-ocl-entry: cubehash512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "cubehash.cl"

__kernel void cubehash512_80(__global const uchar* block, const uint nonce_base,
                             __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv;
  hash_t *hash = &avian_priv;

  /* The 64-byte round reads SWAP4(h4[1]), SWAP4(h4[0]) after a swap8 of the
   * incoming hash, and those cancel: swap8 exchanges each word's halves and
   * reverses their bytes, the [1],[0] ordering undoes the exchange and SWAP4
   * the reversal. So in raw terms it is x0 ^= word[0], which is why its
   * terminator block is x0 ^= 0x80. Reading the header raw drops the dance.
   *
   * CubeHash takes 32-byte blocks, so 80 bytes is three: bytes 0 to 31,
   * 32 to 63, then 64 to 79 with the terminator. The 64-byte round has two
   * message blocks and a third of pure padding, so the loop shape is the
   * same and only the third absorption differs.
   */
  #define HW(i) (((__global const uint *) block)[i])

  // cubehash.h1

  sph_u32 x0 = SPH_C32(0x2AEA2A61), x1 = SPH_C32(0x50F494D4), x2 = SPH_C32(0x2D538B8B), x3 = SPH_C32(0x4167D83E);
  sph_u32 x4 = SPH_C32(0x3FEE2313), x5 = SPH_C32(0xC701CF8C), x6 = SPH_C32(0xCC39968E), x7 = SPH_C32(0x50AC5695);
  sph_u32 x8 = SPH_C32(0x4D42C787), x9 = SPH_C32(0xA647A8B3), xa = SPH_C32(0x97CF0BEF), xb = SPH_C32(0x825B4537);
  sph_u32 xc = SPH_C32(0xEEF864D2), xd = SPH_C32(0xF22090C4), xe = SPH_C32(0xD0E5CD33), xf = SPH_C32(0xA23911AE);
  sph_u32 xg = SPH_C32(0xFCD398D9), xh = SPH_C32(0x148FE485), xi = SPH_C32(0x1B017BEF), xj = SPH_C32(0xB6444532);
  sph_u32 xk = SPH_C32(0x6A536159), xl = SPH_C32(0x2FF5781C), xm = SPH_C32(0x91FA7934), xn = SPH_C32(0x0DBADEA9);
  sph_u32 xo = SPH_C32(0xD65C8A2B), xp = SPH_C32(0xA5A70E75), xq = SPH_C32(0xB1C62456), xr = SPH_C32(0xBC796576);
  sph_u32 xs = SPH_C32(0x1921C8F7), xt = SPH_C32(0xE7989AF1), xu = SPH_C32(0x7795D246), xv = SPH_C32(0xD43E3B44);

  x0 ^= HW(0);
  x1 ^= HW(1);
  x2 ^= HW(2);
  x3 ^= HW(3);
  x4 ^= HW(4);
  x5 ^= HW(5);
  x6 ^= HW(6);
  x7 ^= HW(7);

  for (int i = 0; i < 13; i ++)
  {
    SIXTEEN_ROUNDS;

    if (i == 0)
    {
      x0 ^= HW(8);
      x1 ^= HW(9);
      x2 ^= HW(10);
      x3 ^= HW(11);
      x4 ^= HW(12);
      x5 ^= HW(13);
      x6 ^= HW(14);
      x7 ^= HW(15);
    }
    else if(i == 1)
    {
      x0 ^= HW(16);
      x1 ^= HW(17);
      x2 ^= HW(18);
      x3 ^= SWAP4(nonce_base + gid);   /* header bytes 76 to 79 */
      x4 ^= 0x80;
    }
    else if (i == 2)
      xv ^= SPH_C32(1);
  }

  hash->h4[0] = x0;
  hash->h4[1] = x1;
  hash->h4[2] = x2;
  hash->h4[3] = x3;
  hash->h4[4] = x4;
  hash->h4[5] = x5;
  hash->h4[6] = x6;
  hash->h4[7] = x7;
  hash->h4[8] = x8;
  hash->h4[9] = x9;
  hash->h4[10] = xa;
  hash->h4[11] = xb;
  hash->h4[12] = xc;
  hash->h4[13] = xd;
  hash->h4[14] = xe;
  hash->h4[15] = xf;

  hashes[gid] = avian_priv;
  #undef HW
}

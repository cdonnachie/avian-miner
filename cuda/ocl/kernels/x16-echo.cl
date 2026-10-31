/* echo, one X16RT round: 64 bytes in place, per work item.
 *
 * sgminer's search10 from bitblock.cl, which is MIT; see echo.cl for the
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
 * avian-ocl-entry: echo512_64
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "echo.cl"

__kernel void echo512_64(__global hash_t* hashes)
{
  uint gid = get_global_id(0);
  uint offset = get_global_offset(0);
  hash_t hash;
  __global hash_t *hashp = &(hashes[gid-offset]);

  __local sph_u32 AES0[256], AES1[256], AES2[256], AES3[256];

  int init = get_local_id(0);
  int step = get_local_size(0);

  for (int i = init; i < 256; i += step)
  {
    AES0[i] = AES0_C[i];
    AES1[i] = AES1_C[i];
    AES2[i] = AES2_C[i];
    AES3[i] = AES3_C[i];
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  for (int i = 0; i < 8; i++)
    hash.h8[i] = hashes[gid-offset].h8[i];

  // echo
  sph_u64 W00, W01, W10, W11, W20, W21, W30, W31, W40, W41, W50, W51, W60, W61, W70, W71, W80, W81, W90, W91, WA0, WA1, WB0, WB1, WC0, WC1, WD0, WD1, WE0, WE1, WF0, WF1;
  sph_u64 Vb00, Vb01, Vb10, Vb11, Vb20, Vb21, Vb30, Vb31, Vb40, Vb41, Vb50, Vb51, Vb60, Vb61, Vb70, Vb71;
  Vb00 = Vb10 = Vb20 = Vb30 = Vb40 = Vb50 = Vb60 = Vb70 = 512UL;
  Vb01 = Vb11 = Vb21 = Vb31 = Vb41 = Vb51 = Vb61 = Vb71 = 0;

  sph_u32 K0 = 512;
  sph_u32 K1 = 0;
  sph_u32 K2 = 0;
  sph_u32 K3 = 0;

  W00 = Vb00;
  W01 = Vb01;
  W10 = Vb10;
  W11 = Vb11;
  W20 = Vb20;
  W21 = Vb21;
  W30 = Vb30;
  W31 = Vb31;
  W40 = Vb40;
  W41 = Vb41;
  W50 = Vb50;
  W51 = Vb51;
  W60 = Vb60;
  W61 = Vb61;
  W70 = Vb70;
  W71 = Vb71;
  W80 = hash.h8[0];
  W81 = hash.h8[1];
  W90 = hash.h8[2];
  W91 = hash.h8[3];
  WA0 = hash.h8[4];
  WA1 = hash.h8[5];
  WB0 = hash.h8[6];
  WB1 = hash.h8[7];
  WC0 = 0x80;
  WC1 = 0;
  WD0 = 0;
  WD1 = 0;
  WE0 = 0;
  WE1 = 0x200000000000000;
  WF0 = 0x200;
  WF1 = 0;

  for (unsigned u = 0; u < 10; u ++)
    BIG_ROUND;

  hashp->h8[0] = hash.h8[0] ^ Vb00 ^ W00 ^ W80;
  hashp->h8[1] = hash.h8[1] ^ Vb01 ^ W01 ^ W81;
  hashp->h8[2] = hash.h8[2] ^ Vb10 ^ W10 ^ W90;
  hashp->h8[3] = hash.h8[3] ^ Vb11 ^ W11 ^ W91;
  hashp->h8[4] = hash.h8[4] ^ Vb20 ^ W20 ^ WA0;
  hashp->h8[5] = hash.h8[5] ^ Vb21 ^ W21 ^ WA1;
  hashp->h8[6] = hash.h8[6] ^ Vb30 ^ W30 ^ WB0;
  hashp->h8[7] = hash.h8[7] ^ Vb31 ^ W31 ^ WB1;

  barrier(CLK_GLOBAL_MEM_FENCE);
}

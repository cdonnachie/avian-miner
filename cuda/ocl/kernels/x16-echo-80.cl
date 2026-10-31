/* echo, the X16RT first round: the 80-byte header in, 64 bytes out.
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
 * avian-ocl-entry: echo512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "echo.cl"

__kernel void echo512_80(__global const uchar* block, const uint nonce_base,
                         __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  __global hash_t *hashp = &(hashes[gid]);
  #define HD(i) (((__global const ulong *) block)[i])

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


  // echo
  sph_u64 W00, W01, W10, W11, W20, W21, W30, W31, W40, W41, W50, W51, W60, W61, W70, W71, W80, W81, W90, W91, WA0, WA1, WB0, WB1, WC0, WC1, WD0, WD1, WE0, WE1, WF0, WF1;
  sph_u64 Vb00, Vb01, Vb10, Vb11, Vb20, Vb21, Vb30, Vb31, Vb40, Vb41, Vb50, Vb51, Vb60, Vb61, Vb70, Vb71;
  Vb00 = Vb10 = Vb20 = Vb30 = Vb40 = Vb50 = Vb60 = Vb70 = 512UL;
  Vb01 = Vb11 = Vb21 = Vb31 = Vb41 = Vb51 = Vb61 = Vb71 = 0;

  /* The block counter, which sph calls C0 and increments by the bits
   * processed. For a single-block message that is the message length, so 640
   * here where the 64-byte round has 512. Not to be confused with the Vb
   * initialisation above, which encodes the digest size and stays 512. */
  sph_u32 K0 = 640;
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
  W80 = HD(0);
  W81 = HD(1);
  W90 = HD(2);
  W91 = HD(3);
  WA0 = HD(4);
  WA1 = HD(5);
  WB0 = HD(6);
  WB1 = HD(7);
  /* sph's FINAL_BIG is VV[u] ^= buf[u*8] ^ WW[u] ^ WW[u+16], so the first
   * term below is the message BLOCK word, which in the 64-byte round is the
   * incoming hash because there the hash is the message. Here it is the
   * header word. Dropping it rather than replacing it was my first mistake.
   *
   * Two more message words, then the terminator at block byte 80 rather
   * than 64. The nonce is header bytes 76 to 79, the high half of word 9. */
  WC0 = HD(8);
  WC1 = (HD(9) & 0x00000000FFFFFFFFUL)
        | (((ulong) SWAP4(nonce_base + gid)) << 32);
  WD0 = 0x80;
  WD1 = 0;
  WE0 = 0;
  /* Echo carries two length fields, and both read as 512 in the 64-byte
   * round because the digest size and the message length coincide there.
   * Only the message length becomes 640. */
  WE1 = 0x200000000000000;
  WF0 = 0x280;
  WF1 = 0;

  for (unsigned u = 0; u < 10; u ++)
    BIG_ROUND;

  hashp->h8[0] = HD(0) ^ Vb00 ^ W00 ^ W80;
  hashp->h8[1] = HD(1) ^ Vb01 ^ W01 ^ W81;
  hashp->h8[2] = HD(2) ^ Vb10 ^ W10 ^ W90;
  hashp->h8[3] = HD(3) ^ Vb11 ^ W11 ^ W91;
  hashp->h8[4] = HD(4) ^ Vb20 ^ W20 ^ WA0;
  hashp->h8[5] = HD(5) ^ Vb21 ^ W21 ^ WA1;
  hashp->h8[6] = HD(6) ^ Vb30 ^ W30 ^ WB0;
  hashp->h8[7] = HD(7) ^ Vb31 ^ W31 ^ WB1;

  barrier(CLK_GLOBAL_MEM_FENCE);
  #undef HD
}

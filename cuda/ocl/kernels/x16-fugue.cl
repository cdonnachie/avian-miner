/* fugue, one X16RT round: 64 bytes in place, per work item.
 *
 * sgminer's search12 from bitblock.cl, which is MIT; see fugue.cl for the
 * notice it belongs to. bitblock.cl is x15 written as one kernel per stage
 * over a global hash_t array, which is this backend's architecture too, so
 * the body below is upstream's with five changes and no more: the kernel is
 * renamed, sgminer's reqd_work_group_size attribute is dropped, the global
 * offset term is dropped, SWAP8/SWAP4 are stripped where they wrap a hash->
 * load or store, and a barrier is added after the mixtab fill, which upstream
 * races on; see the note at that barrier.
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
 * avian-ocl-entry: fugue512_64
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "fugue.cl"

__kernel void fugue512_64(__global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv = hashes[gid];
  hash_t *hash = &avian_priv;

    for (int i = 0; i < 16; i++) hash->h4[i] = SWAP4(hash->h4[i]);

  // mixtab
  __local sph_u32 mixtab0[256], mixtab1[256], mixtab2[256], mixtab3[256];
  int init = get_local_id(0);
  int step = get_local_size(0);
  for (int i = init; i < 256; i += step)
  {
    mixtab0[i] = mixtab0_c[i];
    mixtab1[i] = mixtab1_c[i];
    mixtab2[i] = mixtab2_c[i];
    mixtab3[i] = mixtab3_c[i];
  }
  /* Not upstream's. sgminer's bitblock.cl fills this table and reads it with
   * no barrier, which is a race: nothing stops one work item reaching the
   * mixtab lookups before another has written its share of the entries. The
   * other table-filling kernels here (echo, shavite, whirlpool, hamsi) all
   * carry this barrier, and fugue is the only one upstream left out.
   *
   * It survived avian-ocl-selftest because that checks one kernel at a time
   * and the work group ends up small enough that each item fills the whole
   * table itself. It showed up the moment a real batch ran all sixteen rounds
   * at 256 items a group: fugue was the one algorithm of the sixteen whose
   * candidates never verified on the CPU. */
  barrier(CLK_LOCAL_MEM_FENCE);

  // fugue
  sph_u32 S00, S01, S02, S03, S04, S05, S06, S07, S08, S09;
  sph_u32 S10, S11, S12, S13, S14, S15, S16, S17, S18, S19;
  sph_u32 S20, S21, S22, S23, S24, S25, S26, S27, S28, S29;
  sph_u32 S30, S31, S32, S33, S34, S35;

  ulong fc_bit_count = (sph_u64) 64 << 3;

  S00 = S01 = S02 = S03 = S04 = S05 = S06 = S07 = S08 = S09 = S10 = S11 = S12 = S13 = S14 = S15 = S16 = S17 = S18 = S19 = 0;
  S20 = SPH_C32(0x8807a57e); S21 = SPH_C32(0xe616af75); S22 = SPH_C32(0xc5d3e4db); S23 = SPH_C32(0xac9ab027);
  S24 = SPH_C32(0xd915f117); S25 = SPH_C32(0xb6eecc54); S26 = SPH_C32(0x06e8020b); S27 = SPH_C32(0x4a92efd1);
  S28 = SPH_C32(0xaac6e2c9); S29 = SPH_C32(0xddb21398); S30 = SPH_C32(0xcae65838); S31 = SPH_C32(0x437f203f);
  S32 = SPH_C32(0x25ea78e7); S33 = SPH_C32(0x951fddd6); S34 = SPH_C32(0xda6ed11d); S35 = SPH_C32(0xe13e3567);

  FUGUE512_3((hash->h4[0x0]), (hash->h4[0x1]), (hash->h4[0x2]));
  FUGUE512_3((hash->h4[0x3]), (hash->h4[0x4]), (hash->h4[0x5]));
  FUGUE512_3((hash->h4[0x6]), (hash->h4[0x7]), (hash->h4[0x8]));
  FUGUE512_3((hash->h4[0x9]), (hash->h4[0xA]), (hash->h4[0xB]));
  FUGUE512_3((hash->h4[0xC]), (hash->h4[0xD]), (hash->h4[0xE]));
  FUGUE512_3((hash->h4[0xF]), as_uint2(fc_bit_count).y, as_uint2(fc_bit_count).x);

  // apply round shift if necessary
  int i;

  for (i = 0; i < 32; i ++)
  {
    ROR3;
    CMIX36(S00, S01, S02, S04, S05, S06, S18, S19, S20);
    SMIX(S00, S01, S02, S03);
  }

  for (i = 0; i < 13; i ++)
  {
    S04 ^= S00;
    S09 ^= S00;
    S18 ^= S00;
    S27 ^= S00;
    ROR9;
    SMIX(S00, S01, S02, S03);
    S04 ^= S00;
    S10 ^= S00;
    S18 ^= S00;
    S27 ^= S00;
    ROR9;
    SMIX(S00, S01, S02, S03);
    S04 ^= S00;
    S10 ^= S00;
    S19 ^= S00;
    S27 ^= S00;
    ROR9;
    SMIX(S00, S01, S02, S03);
    S04 ^= S00;
    S10 ^= S00;
    S19 ^= S00;
    S28 ^= S00;
    ROR8;
    SMIX(S00, S01, S02, S03);
  }

  S04 ^= S00;
  S09 ^= S00;
  S18 ^= S00;
  S27 ^= S00;

  hash->h4[0] = SWAP4(S01);
  hash->h4[1] = SWAP4(S02);
  hash->h4[2] = SWAP4(S03);
  hash->h4[3] = SWAP4(S04);
  hash->h4[4] = SWAP4(S09);
  hash->h4[5] = SWAP4(S10);
  hash->h4[6] = SWAP4(S11);
  hash->h4[7] = SWAP4(S12);
  hash->h4[8] = SWAP4(S18);
  hash->h4[9] = SWAP4(S19);
  hash->h4[10] = SWAP4(S20);
  hash->h4[11] = SWAP4(S21);
  hash->h4[12] = SWAP4(S27);
  hash->h4[13] = SWAP4(S28);
  hash->h4[14] = SWAP4(S29);
  hash->h4[15] = SWAP4(S30);

  hashes[gid] = avian_priv;
}

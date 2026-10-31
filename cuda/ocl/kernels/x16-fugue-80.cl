/* fugue, the X16RT first round: the 80-byte header in, 64 bytes out.
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
 * avian-ocl-entry: fugue512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "fugue.cl"

__kernel void fugue512_80(__global const uchar* block, const uint nonce_base,
                          __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv;
  hash_t *hash = &avian_priv;

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

  ulong fc_bit_count = (sph_u64) 80 << 3;   /* 640, not 512 */

  S00 = S01 = S02 = S03 = S04 = S05 = S06 = S07 = S08 = S09 = S10 = S11 = S12 = S13 = S14 = S15 = S16 = S17 = S18 = S19 = 0;
  S20 = SPH_C32(0x8807a57e); S21 = SPH_C32(0xe616af75); S22 = SPH_C32(0xc5d3e4db); S23 = SPH_C32(0xac9ab027);
  S24 = SPH_C32(0xd915f117); S25 = SPH_C32(0xb6eecc54); S26 = SPH_C32(0x06e8020b); S27 = SPH_C32(0x4a92efd1);
  S28 = SPH_C32(0xaac6e2c9); S29 = SPH_C32(0xddb21398); S30 = SPH_C32(0xcae65838); S31 = SPH_C32(0x437f203f);
  S32 = SPH_C32(0x25ea78e7); S33 = SPH_C32(0x951fddd6); S34 = SPH_C32(0xda6ed11d); S35 = SPH_C32(0xe13e3567);

  /* Twenty message words and two of length is twenty-two absorption steps,
   * where the 64-byte round has eighteen. Seven triples cover twenty-one and
   * the twenty-second takes rotation A, 21 being a multiple of three.
   *
   * The words are read big-endian, the domain the 64-byte round reaches by
   * swapping its incoming hash, so the nonce goes in byte-swapped. */
  FUGUE512_3(DEC32BE(block + 0), DEC32BE(block + 4), DEC32BE(block + 8));
  FUGUE512_3(DEC32BE(block + 12), DEC32BE(block + 16), DEC32BE(block + 20));
  FUGUE512_3(DEC32BE(block + 24), DEC32BE(block + 28), DEC32BE(block + 32));
  FUGUE512_3(DEC32BE(block + 36), DEC32BE(block + 40), DEC32BE(block + 44));
  FUGUE512_3(DEC32BE(block + 48), DEC32BE(block + 52), DEC32BE(block + 56));
  FUGUE512_3(DEC32BE(block + 60), DEC32BE(block + 64), DEC32BE(block + 68));
  FUGUE512_3(DEC32BE(block + 72), SWAP4(nonce_base + gid), as_uint2(fc_bit_count).y);
  FUGUE512_1A(as_uint2(fc_bit_count).x);

  /* The round shift the comment below calls "if necessary", which the
   * 64-byte round never needs. Each absorption step advances the state's
   * position by 24 of 36, so eighteen steps land on 0 and the final phase
   * can assume it; twenty-two land on 24. sph normalises this at close as
   * rms = round_shift * 12 and a rotate of the 36 words, so this does the
   * same by hand.
   *
   * 24 rather than 12, which is the one thing here that was settled by
   * testing rather than derived: the structural claim that a rotation is
   * needed is sound, but the direction had two plausible readings and the
   * selftest decided between them. */
  #define AVIAN_FUGUE_ROR 24
  {
    const sph_u32 t[36] = {
      S00, S01, S02, S03, S04, S05,
      S06, S07, S08, S09, S10, S11,
      S12, S13, S14, S15, S16, S17,
      S18, S19, S20, S21, S22, S23,
      S24, S25, S26, S27, S28, S29,
      S30, S31, S32, S33, S34, S35,
    };
    S00 = t[(0 + AVIAN_FUGUE_ROR) % 36];
    S01 = t[(1 + AVIAN_FUGUE_ROR) % 36];
    S02 = t[(2 + AVIAN_FUGUE_ROR) % 36];
    S03 = t[(3 + AVIAN_FUGUE_ROR) % 36];
    S04 = t[(4 + AVIAN_FUGUE_ROR) % 36];
    S05 = t[(5 + AVIAN_FUGUE_ROR) % 36];
    S06 = t[(6 + AVIAN_FUGUE_ROR) % 36];
    S07 = t[(7 + AVIAN_FUGUE_ROR) % 36];
    S08 = t[(8 + AVIAN_FUGUE_ROR) % 36];
    S09 = t[(9 + AVIAN_FUGUE_ROR) % 36];
    S10 = t[(10 + AVIAN_FUGUE_ROR) % 36];
    S11 = t[(11 + AVIAN_FUGUE_ROR) % 36];
    S12 = t[(12 + AVIAN_FUGUE_ROR) % 36];
    S13 = t[(13 + AVIAN_FUGUE_ROR) % 36];
    S14 = t[(14 + AVIAN_FUGUE_ROR) % 36];
    S15 = t[(15 + AVIAN_FUGUE_ROR) % 36];
    S16 = t[(16 + AVIAN_FUGUE_ROR) % 36];
    S17 = t[(17 + AVIAN_FUGUE_ROR) % 36];
    S18 = t[(18 + AVIAN_FUGUE_ROR) % 36];
    S19 = t[(19 + AVIAN_FUGUE_ROR) % 36];
    S20 = t[(20 + AVIAN_FUGUE_ROR) % 36];
    S21 = t[(21 + AVIAN_FUGUE_ROR) % 36];
    S22 = t[(22 + AVIAN_FUGUE_ROR) % 36];
    S23 = t[(23 + AVIAN_FUGUE_ROR) % 36];
    S24 = t[(24 + AVIAN_FUGUE_ROR) % 36];
    S25 = t[(25 + AVIAN_FUGUE_ROR) % 36];
    S26 = t[(26 + AVIAN_FUGUE_ROR) % 36];
    S27 = t[(27 + AVIAN_FUGUE_ROR) % 36];
    S28 = t[(28 + AVIAN_FUGUE_ROR) % 36];
    S29 = t[(29 + AVIAN_FUGUE_ROR) % 36];
    S30 = t[(30 + AVIAN_FUGUE_ROR) % 36];
    S31 = t[(31 + AVIAN_FUGUE_ROR) % 36];
    S32 = t[(32 + AVIAN_FUGUE_ROR) % 36];
    S33 = t[(33 + AVIAN_FUGUE_ROR) % 36];
    S34 = t[(34 + AVIAN_FUGUE_ROR) % 36];
    S35 = t[(35 + AVIAN_FUGUE_ROR) % 36];
  }

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
  #undef AVIAN_FUGUE_ROR
}

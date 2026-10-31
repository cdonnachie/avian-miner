/* shavite, one X16RT round: 64 bytes in place, per work item.
 *
 * The body is sgminer's shavite stage from darkcoin.cl, which is MIT; see
 * shavite.cl for the notice it belongs to. One change, and only one: the
 * chain's DEC64E is dropped from every load and store, because sgminer holds
 * its inter-stage hash in byte-swapped 64-bit words where our sph does not.
 * doc/building.md has the comparison that establishes it.
 *
 * In place on a per-work-item 64-byte slot, the convention the CUDA kernels
 * already use, so one host driver fits either backend.
 *
 * avian-ocl-entry: shavite512_64
 */

#include "sph-common.cl"
#include "shavite.cl"

__kernel void shavite512_64(__global ulong *hashes)
{
    const uint gid = get_global_id(0);
    __global ulong *slot = hashes + (ulong) gid * 8;
    union { uchar h1[64]; uint h4[16]; ulong h8[8]; } hash;

    /* Filled cooperatively across the work group and fenced, which is what
     * the chain files do; a stage lifted out of one would otherwise lose the
     * tables entirely. 4 KiB of the 48 KiB a card reports. */
    __local sph_u32 AES0[256], AES1[256], AES2[256], AES3[256];
    for (int i = get_local_id(0); i < 256; i += get_local_size(0))
    {
        AES0[i] = AES0_C[i];
        AES1[i] = AES1_C[i];
        AES2[i] = AES2_C[i];
        AES3[i] = AES3_C[i];
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int i = 0; i < 8; i++)
        hash.h8[i] = slot[i];
    {
    // IV
    sph_u32 h0 = SPH_C32(0x72FCCDD8), h1 = SPH_C32(0x79CA4727), h2 = SPH_C32(0x128A077B), h3 = SPH_C32(0x40D55AEC);
    sph_u32 h4 = SPH_C32(0xD1901A06), h5 = SPH_C32(0x430AE307), h6 = SPH_C32(0xB29F5CD1), h7 = SPH_C32(0xDF07FBFC);
    sph_u32 h8 = SPH_C32(0x8E45D73D), h9 = SPH_C32(0x681AB538), hA = SPH_C32(0xBDE86578), hB = SPH_C32(0xDD577E47);
    sph_u32 hC = SPH_C32(0xE275EADE), hD = SPH_C32(0x502D9FCD), hE = SPH_C32(0xB9357178), hF = SPH_C32(0x022A4B9A);

    // state
    sph_u32 rk00, rk01, rk02, rk03, rk04, rk05, rk06, rk07;
    sph_u32 rk08, rk09, rk0A, rk0B, rk0C, rk0D, rk0E, rk0F;
    sph_u32 rk10, rk11, rk12, rk13, rk14, rk15, rk16, rk17;
    sph_u32 rk18, rk19, rk1A, rk1B, rk1C, rk1D, rk1E, rk1F;

    sph_u32 sc_count0 = (64 << 3), sc_count1 = 0, sc_count2 = 0, sc_count3 = 0;

    rk00 = hash.h4[0];
    rk01 = hash.h4[1];
    rk02 = hash.h4[2];
    rk03 = hash.h4[3];
    rk04 = hash.h4[4];
    rk05 = hash.h4[5];
    rk06 = hash.h4[6];
    rk07 = hash.h4[7];
    rk08 = hash.h4[8];
    rk09 = hash.h4[9];
    rk0A = hash.h4[10];
    rk0B = hash.h4[11];
    rk0C = hash.h4[12];
    rk0D = hash.h4[13];
    rk0E = hash.h4[14];
    rk0F = hash.h4[15];
    rk10 = 0x80;
    rk11 = rk12 = rk13 = rk14 = rk15 = rk16 = rk17 = rk18 = rk19 = rk1A = 0;
    rk1B = 0x2000000;
    rk1C = rk1D = rk1E = 0;
    rk1F = 0x2000000;

    c512(buf);

    hash.h4[0] = h0;
    hash.h4[1] = h1;
    hash.h4[2] = h2;
    hash.h4[3] = h3;
    hash.h4[4] = h4;
    hash.h4[5] = h5;
    hash.h4[6] = h6;
    hash.h4[7] = h7;
    hash.h4[8] = h8;
    hash.h4[9] = h9;
    hash.h4[10] = hA;
    hash.h4[11] = hB;
    hash.h4[12] = hC;
    hash.h4[13] = hD;
    hash.h4[14] = hE;
    hash.h4[15] = hF;
    }
    for (int i = 0; i < 8; i++)
        slot[i] = hash.h8[i];
}

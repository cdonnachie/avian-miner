/* groestl, one X16RT round: 64 bytes in place, per work item.
 *
 * The body is sgminer's groestl stage from darkcoin.cl, which is MIT; see
 * groestl.cl for the notice it belongs to. One change, and only one: the
 * chain's DEC64E is dropped from every load and store, because sgminer holds
 * its inter-stage hash in byte-swapped 64-bit words where our sph does not.
 * doc/building.md has the comparison that establishes it.
 *
 * In place on a per-work-item 64-byte slot, the convention the CUDA kernels
 * already use, so one host driver fits either backend.
 *
 * avian-ocl-entry: groestl512_64
 */

#include "sph-common.cl"
#include "groestl.cl"

__kernel void groestl512_64(__global ulong *hashes)
{
    const uint gid = get_global_id(0);
    __global ulong *slot = hashes + (ulong) gid * 8;
    union { uchar h1[64]; uint h4[16]; ulong h8[8]; } hash;

    for (int i = 0; i < 8; i++)
        hash.h8[i] = slot[i];
    {
    sph_u64 H[16];
    for (unsigned int u = 0; u < 15; u ++)
      H[u] = 0;
  #if USE_LE
    H[15] = ((sph_u64)(512 & 0xFF) << 56) | ((sph_u64)(512 & 0xFF00) << 40);
  #else
    H[15] = (sph_u64)512;
  #endif

    sph_u64 g[16], m[16];
    m[0] = hash.h8[0];
    m[1] = hash.h8[1];
    m[2] = hash.h8[2];
    m[3] = hash.h8[3];
    m[4] = hash.h8[4];
    m[5] = hash.h8[5];
    m[6] = hash.h8[6];
    m[7] = hash.h8[7];
    for (unsigned int u = 0; u < 16; u ++)
      g[u] = m[u] ^ H[u];
    m[8] = 0x80; g[8] = m[8] ^ H[8];
    m[9] = 0; g[9] = m[9] ^ H[9];
    m[10] = 0; g[10] = m[10] ^ H[10];
    m[11] = 0; g[11] = m[11] ^ H[11];
    m[12] = 0; g[12] = m[12] ^ H[12];
    m[13] = 0; g[13] = m[13] ^ H[13];
    m[14] = 0; g[14] = m[14] ^ H[14];
    m[15] = 0x100000000000000; g[15] = m[15] ^ H[15];
    PERM_BIG_P(g);
    PERM_BIG_Q(m);
    for (unsigned int u = 0; u < 16; u ++)
      H[u] ^= g[u] ^ m[u];
    sph_u64 xH[16];
    for (unsigned int u = 0; u < 16; u ++)
      xH[u] = H[u];
    PERM_BIG_P(xH);
    for (unsigned int u = 0; u < 16; u ++)
      H[u] ^= xH[u];
    for (unsigned int u = 0; u < 8; u ++)
      hash.h8[u] = H[u + 8];
    }
    for (int i = 0; i < 8; i++)
        slot[i] = hash.h8[i];
}

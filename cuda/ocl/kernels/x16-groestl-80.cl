/* groestl, the X16RT first round: the 80-byte header in, 64 bytes out.
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
 * avian-ocl-entry: groestl512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "groestl.cl"

__kernel void groestl512_80(__global const uchar *block,
                            const uint nonce_base,
                            __global hash_t *hashes)
{
    const uint gid = get_global_id(0);
    /* Before anything else, and every work item reaches it: the macro
     * ends in a barrier. See groestl.cl for why these are not left in
     * __constant. */
    GROESTL_STAGE_TABLES();
    hash_t avian_priv;
    hash_t *hash = &avian_priv;
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
    m[0] = ((__global const ulong *) block)[0];
    m[1] = ((__global const ulong *) block)[1];
    m[2] = ((__global const ulong *) block)[2];
    m[3] = ((__global const ulong *) block)[3];
    m[4] = ((__global const ulong *) block)[4];
    m[5] = ((__global const ulong *) block)[5];
    m[6] = ((__global const ulong *) block)[6];
    m[7] = ((__global const ulong *) block)[7];
    m[8] = ((__global const ulong *) block)[8];
    m[9] = ((__global const ulong *) block)[9];
    /* The nonce at header bytes 76 to 79. Groestl reads the message
     * natively, so those four bytes are word 9's high half. */
    m[9] &= 0x00000000FFFFFFFFUL;
    m[9] |= ((ulong) (nonce_base + gid)) << 32;
    for (unsigned int u = 0; u < 16; u ++)
      g[u] = m[u] ^ H[u];
    m[10] = 0x80; g[10] = m[10] ^ H[10];
    m[11] = 0; g[11] = m[11] ^ H[11];
    m[12] = 0; g[12] = m[12] ^ H[12];
    m[13] = 0; g[13] = m[13] ^ H[13];
    m[14] = 0; g[14] = m[14] ^ H[14];
    /* Unchanged: this word is a byte-swapped block count, and 80
     * bytes plus padding is still one 128-byte block. */
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
      hash->h8[u] = H[u + 8];
    }
    hashes[gid] = avian_priv;
}

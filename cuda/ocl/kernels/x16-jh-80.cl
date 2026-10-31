/* JH-512, one X16RT round: 64 bytes in place, per work item.
 *
 * The body is sgminer's JH stage from darkcoin.cl, which is MIT; see jh.cl
 * for the notice it belongs to. One change, and only one: the chain's
 * DEC64E is dropped from every load and store. sgminer holds its inter-stage
 * hash in byte-swapped 64-bit words, where our sph does not, and dropping
 * the swap is what makes this byte-exact against sph_jh512 on raw bytes.
 * doc/building.md has the four-way comparison that establishes it.
 *
 * In place on a per-work-item 64-byte slot, which is the convention the CUDA
 * kernels already use (x17_sha512_gpu_hash_64 and its siblings), so the host
 * driver is the same shape for either backend.
 *
 * avian-ocl-entry: jh512_80
 * Read by cuda/ocl/mkkernels.sh, which cannot derive the entry point from the
 * file name and must not guess it.
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "jh.cl"

__kernel void jh512_80(__global const uchar *block,
                       const uint nonce_base,
                       __global hash_t *hashes)
{
    const uint gid = get_global_id(0);
    hash_t avian_priv;
    hash_t *hash = &avian_priv;
    {
    sph_u64 h0h = C64e(0x6fd14b963e00aa17), h0l = C64e(0x636a2e057a15d543), h1h = C64e(0x8a225e8d0c97ef0b), h1l = C64e(0xe9341259f2b3c361), h2h = C64e(0x891da0c1536f801e), h2l = C64e(0x2aa9056bea2b6d80), h3h = C64e(0x588eccdb2075baa6), h3l = C64e(0xa90f3a76baf83bf7);
    sph_u64 h4h = C64e(0x0169e60541e34a69), h4l = C64e(0x46b58a8e2e6fe65a), h5h = C64e(0x1047a7d0c1843c24), h5l = C64e(0x3b6e71b12d5ac199), h6h = C64e(0xcf57f6ec9db1f856), h6l = C64e(0xa706887c5716b156), h7h = C64e(0xe3c2fcdfe68517fb), h7l = C64e(0x545a4678cc8cdd4b);
    sph_u64 tmp;

    /* Three blocks, not two, which is what our own
     * cuda/upstream/quark/cuda_jh512.cu does for 80 bytes and the first
     * thing I got wrong here. A 64-byte message fits its terminator and
     * its 128-bit length into one padding block; an 80-byte message does
     * not, so the length gets a block to itself.
     *
     *   block 1   header bytes 0 to 63
     *   block 2   header bytes 64 to 79, terminator at block offset 16
     *   block 3   the length alone
     *
     * JH compresses each block as first half ^= M, E8, second half ^= M.
     * Words are raw little-endian reads, the domain this round already
     * works in, so the nonce lands in the high half of header word 9.
     */
    const ulong avian_nonce = (ulong) (nonce_base + gid);
    sph_u64 m0 = ((__global const ulong *) block)[8];
    sph_u64 m1 = (((__global const ulong *) block)[9] & 0x00000000FFFFFFFFUL)
                 | (avian_nonce << 32);

    /* block 1 */
    h0h ^= ((__global const ulong *) block)[0];
    h0l ^= ((__global const ulong *) block)[1];
    h1h ^= ((__global const ulong *) block)[2];
    h1l ^= ((__global const ulong *) block)[3];
    h2h ^= ((__global const ulong *) block)[4];
    h2l ^= ((__global const ulong *) block)[5];
    h3h ^= ((__global const ulong *) block)[6];
    h3l ^= ((__global const ulong *) block)[7];
    E8;
    h4h ^= ((__global const ulong *) block)[0];
    h4l ^= ((__global const ulong *) block)[1];
    h5h ^= ((__global const ulong *) block)[2];
    h5l ^= ((__global const ulong *) block)[3];
    h6h ^= ((__global const ulong *) block)[4];
    h6l ^= ((__global const ulong *) block)[5];
    h7h ^= ((__global const ulong *) block)[6];
    h7l ^= ((__global const ulong *) block)[7];

    /* block 2: sixteen bytes of header, then 0x80 at block offset 16,
     * which is the low byte of the third word. */
    h0h ^= m0;
    h0l ^= m1;
    h1h ^= 0x80;
    E8;
    h4h ^= m0;
    h4l ^= m1;
    h5h ^= 0x80;

    /* block 3: the length alone. 640 bits big-endian in the block's last
     * four bytes is 0x80020000 as a 32-bit word, and those bytes are the
     * high half of this 64-bit word. The 64-byte round has 512 the same
     * way, as 0x2000000000000. */
    h3l ^= 0x8002000000000000UL;
    E8;
    h7l ^= 0x8002000000000000UL;
    hash->h8[0] = h4h;
    hash->h8[1] = h4l;
    hash->h8[2] = h5h;
    hash->h8[3] = h5l;
    hash->h8[4] = h6h;
    hash->h8[5] = h6l;
    hash->h8[6] = h7h;
    hash->h8[7] = h7l;
    }
    hashes[gid] = avian_priv;
}

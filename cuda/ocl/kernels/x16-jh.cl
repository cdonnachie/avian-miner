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
 * avian-ocl-entry: jh512_64
 * Read by cuda/ocl/mkkernels.sh, which cannot derive the entry point from the
 * file name and must not guess it.
 */

#include "sph-common.cl"
#include "jh.cl"

__kernel void jh512_64(__global ulong *hashes)
{
    const uint gid = get_global_id(0);
    __global ulong *slot = hashes + (ulong) gid * 8;
    union { uchar h1[64]; uint h4[16]; ulong h8[8]; } hash;

    for (int i = 0; i < 8; i++)
        hash.h8[i] = slot[i];
    {
    sph_u64 h0h = C64e(0x6fd14b963e00aa17), h0l = C64e(0x636a2e057a15d543), h1h = C64e(0x8a225e8d0c97ef0b), h1l = C64e(0xe9341259f2b3c361), h2h = C64e(0x891da0c1536f801e), h2l = C64e(0x2aa9056bea2b6d80), h3h = C64e(0x588eccdb2075baa6), h3l = C64e(0xa90f3a76baf83bf7);
    sph_u64 h4h = C64e(0x0169e60541e34a69), h4l = C64e(0x46b58a8e2e6fe65a), h5h = C64e(0x1047a7d0c1843c24), h5l = C64e(0x3b6e71b12d5ac199), h6h = C64e(0xcf57f6ec9db1f856), h6l = C64e(0xa706887c5716b156), h7h = C64e(0xe3c2fcdfe68517fb), h7l = C64e(0x545a4678cc8cdd4b);
    sph_u64 tmp;

    for(int i = 0; i < 2; i++)
    {
      if (i == 0) {
        h0h ^= hash.h8[0];
        h0l ^= hash.h8[1];
        h1h ^= hash.h8[2];
        h1l ^= hash.h8[3];
        h2h ^= hash.h8[4];
        h2l ^= hash.h8[5];
        h3h ^= hash.h8[6];
        h3l ^= hash.h8[7];
      } else if(i == 1) {
        h4h ^= hash.h8[0];
        h4l ^= hash.h8[1];
        h5h ^= hash.h8[2];
        h5l ^= hash.h8[3];
        h6h ^= hash.h8[4];
        h6l ^= hash.h8[5];
        h7h ^= hash.h8[6];
        h7l ^= hash.h8[7];

        h0h ^= 0x80;
        h3l ^= 0x2000000000000;
      }
      E8;
    }
    h4h ^= 0x80;
    h7l ^= 0x2000000000000;

    hash.h8[0] = h4h;
    hash.h8[1] = h4l;
    hash.h8[2] = h5h;
    hash.h8[3] = h5l;
    hash.h8[4] = h6h;
    hash.h8[5] = h6l;
    hash.h8[6] = h7h;
    hash.h8[7] = h7l;
    }
    for (int i = 0; i < 8; i++)
        slot[i] = hash.h8[i];
}

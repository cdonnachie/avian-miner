/* The target check at the end of a batch.
 *
 * Not one of the sixteen rounds. After the chain has run, every work item
 * holds its own 64-byte hash, and this picks out the ones worth sending back
 * to the miner.
 *
 * Only the top 64 bits are compared, which is deliberate rather than lazy.
 * The ABI in cuda/avian-cuda-api.h requires the miner to re-hash every
 * candidate on the CPU before submitting, so a false positive here costs one
 * CPU hash and nothing else, while a false negative would lose a block. A
 * 64-bit compare cannot produce a false negative: if the full 256-bit hash is
 * below the target then its top 64 bits are at or below the target's.
 *
 * The hash is 32 bytes little-endian, so its most significant eight bytes are
 * h8[3]. sgminer's chain files compare the same word.
 *
 * out[0] is the count, out[1..] the nonces. The count is incremented
 * atomically because any number of work items may qualify at once, and
 * writing past max_out is dropped rather than growing the buffer: more
 * candidates than that in one batch means the target is absurdly easy, which
 * only happens on a test chain.
 *
 * avian-ocl-entry: x16_check
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"

__kernel void x16_check(__global const hash_t *hashes, const uint nonce_base,
                        const ulong target, __global uint *out,
                        const uint max_out)
{
    const uint gid = get_global_id(0);

    if (hashes[gid].h8[3] <= target)
    {
        const uint slot = atomic_inc(&out[0]);

        if (slot < max_out)
            out[1 + slot] = nonce_base + gid;
    }
}

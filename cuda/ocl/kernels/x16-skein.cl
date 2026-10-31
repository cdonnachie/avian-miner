/* skein, one X16RT round: 64 bytes in place, per work item.
 *
 * sgminer's search3 from bitblock.cl, which is MIT; see skein.cl for the
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
 * avian-ocl-entry: skein512_64
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "skein.cl"

__kernel void skein512_64(__global hash_t* hashes)
{
  uint gid = get_global_id(0);
  __global hash_t *hash = &(hashes[gid]);

  // skein

  sph_u64 h0 = SPH_C64(0x4903ADFF749C51CE), h1 = SPH_C64(0x0D95DE399746DF03), h2 = SPH_C64(0x8FD1934127C79BCE), h3 = SPH_C64(0x9A255629FF352CB1), h4 = SPH_C64(0x5DB62599DF6CA7B0), h5 = SPH_C64(0xEABE394CA9D5C3F4), h6 = SPH_C64(0x991112C71A75B523), h7 = SPH_C64(0xAE18A40B660FCC33);
  sph_u64 m0, m1, m2, m3, m4, m5, m6, m7;
  sph_u64 bcount = 0;

  m0 = hash->h8[0];
  m1 = hash->h8[1];
  m2 = hash->h8[2];
  m3 = hash->h8[3];
  m4 = hash->h8[4];
  m5 = hash->h8[5];
  m6 = hash->h8[6];
  m7 = hash->h8[7];

  UBI_BIG(480, 64);

  bcount = 0;
  m0 = m1 = m2 = m3 = m4 = m5 = m6 = m7 = 0;

  UBI_BIG(510, 8);

  hash->h8[0] = h0;
  hash->h8[1] = h1;
  hash->h8[2] = h2;
  hash->h8[3] = h3;
  hash->h8[4] = h4;
  hash->h8[5] = h5;
  hash->h8[6] = h6;
  hash->h8[7] = h7;

  barrier(CLK_GLOBAL_MEM_FENCE);
}

/* skein, the X16RT first round: the 80-byte header in, 64 bytes out.
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
 * avian-ocl-entry: skein512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "skein.cl"

__kernel void skein512_80(__global const uchar* block, const uint nonce_base,
                          __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv;
  hash_t *hash = &avian_priv;

  // skein

  sph_u64 h0 = SPH_C64(0x4903ADFF749C51CE), h1 = SPH_C64(0x0D95DE399746DF03), h2 = SPH_C64(0x8FD1934127C79BCE), h3 = SPH_C64(0x9A255629FF352CB1), h4 = SPH_C64(0x5DB62599DF6CA7B0), h5 = SPH_C64(0xEABE394CA9D5C3F4), h6 = SPH_C64(0x991112C71A75B523), h7 = SPH_C64(0xAE18A40B660FCC33);
  sph_u64 m0, m1, m2, m3, m4, m5, m6, m7;
  sph_u64 bcount = 0;

  /* Skein takes 64-byte blocks, so 80 bytes is two message blocks where 64
   * is one. UBI_BIG(etype, extra) builds the tweak as
   * t0 = (bcount << 6) + extra and t1 = etype << 55, so etype carries the
   * FINAL and FIRST flags and the block type:
   *
   *   480  0xF000000000000000  FINAL | FIRST | MSG   (64 bytes, one block)
   *   224  0x7000000000000000  FIRST | MSG           (bytes 0 to 63)
   *   352  0xB000000000000000  FINAL | MSG           (bytes 64 to 79)
   *   510  0xFF00000000000000  FINAL | FIRST | OUT   (the output transform)
   *
   * 352 with t0 = 80 is what cuda/upstream/quark/cuda_skein512.cu uses for
   * its 80-byte final block, as t1 = 0xB000000000000000 and t0 = 0x50. */

  m0 = ((__global const ulong *) block)[0];
  m1 = ((__global const ulong *) block)[1];
  m2 = ((__global const ulong *) block)[2];
  m3 = ((__global const ulong *) block)[3];
  m4 = ((__global const ulong *) block)[4];
  m5 = ((__global const ulong *) block)[5];
  m6 = ((__global const ulong *) block)[6];
  m7 = ((__global const ulong *) block)[7];

  UBI_BIG(224, 64);

  /* The second block holds sixteen bytes of header and nothing else, so
   * t0 reaches 80 as bcount 1 giving 64 plus extra 16. The nonce is header
   * bytes 76 to 79, the high half of word 9. */
  bcount = 1;
  m0 = ((__global const ulong *) block)[8];
  m1 = (((__global const ulong *) block)[9] & 0x00000000FFFFFFFFUL)
       | (((ulong) SWAP4(nonce_base + gid)) << 32);
  m2 = m3 = m4 = m5 = m6 = m7 = 0;

  UBI_BIG(352, 16);

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

  hashes[gid] = avian_priv;
}

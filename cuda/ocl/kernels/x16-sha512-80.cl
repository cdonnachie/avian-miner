/* SHA-512, one X16RT round: 64 bytes in place, per work item.
 *
 * Written rather than ported. The sgminer kernels this backend draws on
 * predate x16r, which added SHA-512 as the sixteenth algorithm, so there is
 * no .cl for it to take.
 *
 * The textbook round form, not the rotating-register one our CUDA kernel
 * uses. That one is faster and considerably harder to port without a subtle
 * error, and this is the only algorithm here whose correctness rests on new
 * code rather than on upstream's. The macros and the round constants are in
 * sha512.cl, shared with the 80-byte kernel.
 *
 * A 64-byte message is exactly one block: 64 bytes, the 0x80 terminator, and
 * the 128-bit length, which is 64 + 1 + 16 = 81 of the 128 available. So
 * there is no block loop and the padding is constant.
 *
 * avian-ocl-entry: sha512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"
#include "sha512.cl"

__kernel void sha512_80(__global const uchar* block, const uint nonce_base,
                        __global hash_t* hashes)
{
  uint gid = get_global_id(0);
  hash_t avian_priv;
  hash_t *hash = &avian_priv;

  ulong W[80];
  ulong a, b, c, d, e, f, g, h;
  ulong s[8];

  /* SHA-512 reads its message as big-endian 64-bit words, which is also what
   * sph_sha512 does, so the raw bytes need swapping on a little-endian
   * device and need no further conversion at the boundary. */
  for (int i = 0; i < 10; i++)
    W[i] = DEC64BE(block + i * 8);

  /* The nonce at header bytes 76 to 79. W[9] holds bytes 72 to 79 read
   * big-endian, so its low half is those four bytes, and SWAP4 puts the
   * nonce there little-endian, where the miner's header has it. */
  W[9] &= 0xFFFFFFFF00000000UL;
  W[9] ^= nonce_base + gid;

  W[10] = 0x8000000000000000UL;    /* the terminator, now past ten words */
  for (int i = 11; i < 15; i++)
    W[i] = 0;
  W[15] = 640;                     /* 80 bytes */

  for (int i = 16; i < 80; i++)
    W[i] = W[i - 16] + SHA512_SSG0(W[i - 15]) + W[i - 7] + SHA512_SSG1(W[i - 2]);

  s[0] = a = 0x6A09E667F3BCC908UL;
  s[1] = b = 0xBB67AE8584CAA73BUL;
  s[2] = c = 0x3C6EF372FE94F82BUL;
  s[3] = d = 0xA54FF53A5F1D36F1UL;
  s[4] = e = 0x510E527FADE682D1UL;
  s[5] = f = 0x9B05688C2B3E6C1FUL;
  s[6] = g = 0x1F83D9ABFB41BD6BUL;
  s[7] = h = 0x5BE0CD19137E2179UL;

  for (int i = 0; i < 80; i++)
  {
    ulong t1 = h + SHA512_BSG1(e) + SHA512_CH(e, f, g) + SHA512_K[i] + W[i];
    ulong t2 = SHA512_BSG0(a) + SHA512_MAJ(a, b, c);

    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  s[0] += a; s[1] += b; s[2] += c; s[3] += d;
  s[4] += e; s[5] += f; s[6] += g; s[7] += h;

  for (int i = 0; i < 8; i++)
    hash->h8[i] = SWAP8(s[i]);

  hashes[gid] = avian_priv;
}

/* SHA-512, one X16RT round: 64 bytes in place, per work item.
 *
 * Written rather than ported. The sgminer kernels this backend draws on
 * predate x16r, which added SHA-512 as the sixteenth algorithm, so there is
 * no .cl for it to take.
 *
 * The textbook round form, not the rotating-register one our CUDA kernel
 * uses. That one is faster and considerably harder to port without a subtle
 * error, and this is the only algorithm here whose correctness rests on new
 * code rather than on upstream's. The round constants below are extracted
 * from cuda/upstream/x17/cuda_x17_sha512.cu by cuda/ocl/mkkernels.sh's
 * companion script, so they are not retyped.
 *
 * A 64-byte message is exactly one block: 64 bytes, the 0x80 terminator, and
 * the 128-bit length, which is 64 + 1 + 16 = 81 of the 128 available. So
 * there is no block loop and the padding is constant.
 *
 * avian-ocl-entry: sha512_80
 */

#include "sph-common.cl"
#include "avian-ocl-common.cl"

/* rotate() is a left rotate, so a right rotate by n is a left rotate by
 * 64 - n. The count has to be the same width as the value. */
#define ROR64(x, n)   rotate((ulong)(x), (ulong)(64 - (n)))

#define BSG0(x)  (ROR64(x, 28) ^ ROR64(x, 34) ^ ROR64(x, 39))
#define BSG1(x)  (ROR64(x, 14) ^ ROR64(x, 18) ^ ROR64(x, 41))
#define SSG0(x)  (ROR64(x,  1) ^ ROR64(x,  8) ^ ((x) >> 7))
#define SSG1(x)  (ROR64(x, 19) ^ ROR64(x, 61) ^ ((x) >> 6))

/* bitselect(a, b, c) is "c ? b : a" per bit, which is one instruction on AMD
 * (v_bfi_b32) and the right way to write these two on a GPU. */
#define CH(x, y, z)   bitselect(z, y, x)
#define MAJ(x, y, z)  bitselect(x, y, (x) ^ (z))

__constant static const ulong SHA512_K[80] = {
    0x428A2F98D728AE22UL, 0x7137449123EF65CDUL, 0xB5C0FBCFEC4D3B2FUL, 0xE9B5DBA58189DBBCUL,
    0x3956C25BF348B538UL, 0x59F111F1B605D019UL, 0x923F82A4AF194F9BUL, 0xAB1C5ED5DA6D8118UL,
    0xD807AA98A3030242UL, 0x12835B0145706FBEUL, 0x243185BE4EE4B28CUL, 0x550C7DC3D5FFB4E2UL,
    0x72BE5D74F27B896FUL, 0x80DEB1FE3B1696B1UL, 0x9BDC06A725C71235UL, 0xC19BF174CF692694UL,
    0xE49B69C19EF14AD2UL, 0xEFBE4786384F25E3UL, 0x0FC19DC68B8CD5B5UL, 0x240CA1CC77AC9C65UL,
    0x2DE92C6F592B0275UL, 0x4A7484AA6EA6E483UL, 0x5CB0A9DCBD41FBD4UL, 0x76F988DA831153B5UL,
    0x983E5152EE66DFABUL, 0xA831C66D2DB43210UL, 0xB00327C898FB213FUL, 0xBF597FC7BEEF0EE4UL,
    0xC6E00BF33DA88FC2UL, 0xD5A79147930AA725UL, 0x06CA6351E003826FUL, 0x142929670A0E6E70UL,
    0x27B70A8546D22FFCUL, 0x2E1B21385C26C926UL, 0x4D2C6DFC5AC42AEDUL, 0x53380D139D95B3DFUL,
    0x650A73548BAF63DEUL, 0x766A0ABB3C77B2A8UL, 0x81C2C92E47EDAEE6UL, 0x92722C851482353BUL,
    0xA2BFE8A14CF10364UL, 0xA81A664BBC423001UL, 0xC24B8B70D0F89791UL, 0xC76C51A30654BE30UL,
    0xD192E819D6EF5218UL, 0xD69906245565A910UL, 0xF40E35855771202AUL, 0x106AA07032BBD1B8UL,
    0x19A4C116B8D2D0C8UL, 0x1E376C085141AB53UL, 0x2748774CDF8EEB99UL, 0x34B0BCB5E19B48A8UL,
    0x391C0CB3C5C95A63UL, 0x4ED8AA4AE3418ACBUL, 0x5B9CCA4F7763E373UL, 0x682E6FF3D6B2B8A3UL,
    0x748F82EE5DEFB2FCUL, 0x78A5636F43172F60UL, 0x84C87814A1F0AB72UL, 0x8CC702081A6439ECUL,
    0x90BEFFFA23631E28UL, 0xA4506CEBDE82BDE9UL, 0xBEF9A3F7B2C67915UL, 0xC67178F2E372532BUL,
    0xCA273ECEEA26619CUL, 0xD186B8C721C0C207UL, 0xEADA7DD6CDE0EB1EUL, 0xF57D4F7FEE6ED178UL,
    0x06F067AA72176FBAUL, 0x0A637DC5A2C898A6UL, 0x113F9804BEF90DAEUL, 0x1B710B35131C471BUL,
    0x28DB77F523047D84UL, 0x32CAAB7B40C72493UL, 0x3C9EBE0A15C9BEBCUL, 0x431D67C49C100D4CUL,
    0x4CC5D4BECB3E42B6UL, 0x597F299CFC657E2AUL, 0x5FCB6FAB3AD6FAECUL, 0x6C44198C4A475817UL,
};

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
  W[9] ^= SWAP4(nonce_base + gid);

  W[10] = 0x8000000000000000UL;    /* the terminator, now past ten words */
  for (int i = 11; i < 15; i++)
    W[i] = 0;
  W[15] = 640;                     /* 80 bytes */

  for (int i = 16; i < 80; i++)
    W[i] = W[i - 16] + SSG0(W[i - 15]) + W[i - 7] + SSG1(W[i - 2]);

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
    ulong t1 = h + BSG1(e) + CH(e, f, g) + SHA512_K[i] + W[i];
    ulong t2 = BSG0(a) + MAJ(a, b, c);

    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  s[0] += a; s[1] += b; s[2] += c; s[3] += d;
  s[4] += e; s[5] += f; s[6] += g; s[7] += h;

  for (int i = 0; i < 8; i++)
    hash->h8[i] = SWAP8(s[i]);

  hashes[gid] = avian_priv;
}

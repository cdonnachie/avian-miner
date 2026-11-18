/* Bitsliced Groestl-512 in registers, two hashes a thread.
 *
 * s[row * 8 + plane] holds one row of the 8x16 byte state for both hashes:
 * bit 2c+h is bit `plane` of the byte at (row, column c) of hash h. A row is
 * one word, so ShiftBytes is a rotate and MixBytes needs no shuffles, where
 * the quad kernel paid a third of its time moving rows between four threads.
 * The layout is the one in tpfuemp's ccminer (groestl512_x2_device.cuh); the
 * gate networks are generated here, see scripts/gen-groestl-bitslice.py.
 *
 * The kernel is bound by integer throughput alone: on a 3090 the rate is the
 * card's 8.8e12 thread-instructions a second over the count per hash, at any
 * occupancy. About 215 registers a thread, so two blocks of 128 fill an SM.
 */
#ifndef GROESTL512_BS_H
#define GROESTL512_BS_H

#include "avian-ptx-portable.h"
#include "quark/groestl512_bs_gen.h"

#define GBS_TPB 128
#define GBS_ST_WORDS 64		/* shared words a thread, for gbs_compress */

#define GBS_SWAP(x, y, m, n) { const uint32_t t_ = ((x) ^ ((y) << (n))) & (m); (x) ^= t_; (y) ^= t_ >> (n); }

/* Eight words: exchanges the word index with the low three bits of the bit
 * index. Its own inverse. */
__device__ __forceinline__ void gbs_transpose(uint32_t *const d)
{
	GBS_SWAP(d[0], d[1], 0xaaaaaaaau, 1); GBS_SWAP(d[2], d[3], 0xaaaaaaaau, 1);
	GBS_SWAP(d[4], d[5], 0xaaaaaaaau, 1); GBS_SWAP(d[6], d[7], 0xaaaaaaaau, 1);
	GBS_SWAP(d[0], d[2], 0xccccccccu, 2); GBS_SWAP(d[1], d[3], 0xccccccccu, 2);
	GBS_SWAP(d[4], d[6], 0xccccccccu, 2); GBS_SWAP(d[5], d[7], 0xccccccccu, 2);
	GBS_SWAP(d[0], d[4], 0xf0f0f0f0u, 4); GBS_SWAP(d[1], d[5], 0xf0f0f0f0u, 4);
	GBS_SWAP(d[2], d[6], 0xf0f0f0f0u, 4); GBS_SWAP(d[3], d[7], 0xf0f0f0f0u, 4);
}

/* b0, b1: two padded 128-byte blocks, 32 words each. */
__device__ __forceinline__ void gbs_load(const uint32_t *const b0, const uint32_t *const b1, uint32_t *const s)
{
#pragma unroll
	for (int r = 0; r < 8; r++) {
		const int rw = r >> 2;
		const uint32_t sel = (r & 3) | ((4 + (r & 3)) << 4);
#pragma unroll
		for (int k = 0; k < 8; k++) {
			const uint32_t *const b = (k & 1) ? b1 : b0;
			const int cc = k >> 1;
			const uint32_t lo = __byte_perm(b[2 * cc + rw], b[8 + 2 * cc + rw], sel);
			const uint32_t hi = __byte_perm(b[16 + 2 * cc + rw], b[24 + 2 * cc + rw], sel);
			s[r * 8 + k] = __byte_perm(lo, hi, 0x5410);
		}
		gbs_transpose(&s[r * 8]);
	}
}

/* The digests, columns 8..15 of both states. s is consumed. */
__device__ __forceinline__ void gbs_store(uint32_t *const s, uint32_t *const o0, uint32_t *const o1)
{
#pragma unroll
	for (int r = 0; r < 8; r++) gbs_transpose(&s[r * 8]);
#pragma unroll
	for (int h = 0; h < 2; h++) {
		uint32_t *const o = h ? o1 : o0;
#pragma unroll
		for (int w = 0; w < 16; w++) {
			const int c = 8 + (w >> 1), k = 2 * (c & 3) + h, n = c >> 2, r0 = 4 * (w & 1);
			const uint32_t sel = n | ((4 + n) << 4);
			const uint32_t lo = __byte_perm(s[r0 * 8 + k], s[(r0 + 1) * 8 + k], sel);
			const uint32_t hi = __byte_perm(s[(r0 + 2) * 8 + k], s[(r0 + 3) * 8 + k], sel);
			o[w] = __byte_perm(lo, hi, 0x5410);
		}
	}
}

/* Round constant on one row: the column number in the high nibble, the round
 * in the low. Q's all-ones part is in gbs_sbox_q. */
__device__ __forceinline__ void gbs_rc(uint32_t *const row, const uint32_t r)
{
#pragma unroll
	for (int p = 0; p < 4; p++) row[p] ^= 0u - ((r >> p) & 1u);
	row[4] ^= 0xCCCCCCCCu; row[5] ^= 0xF0F0F0F0u; row[6] ^= 0xFF00FF00u; row[7] ^= 0xFFFF0000u;
}

/* SubBytes on a row, then its ShiftBytes: n columns is 2n bits. */
#define GBS_ROW(F, s, i, n) { \
	F(s[(i) * 8], s[(i) * 8 + 1], s[(i) * 8 + 2], s[(i) * 8 + 3], s[(i) * 8 + 4], s[(i) * 8 + 5], s[(i) * 8 + 6], s[(i) * 8 + 7]); \
	if (n) { for (int p_ = 0; p_ < 8; p_++) s[(i) * 8 + p_] = ROTR32(s[(i) * 8 + p_], 2 * (n)); } }

__device__ __forceinline__ void gbs_perm_p(uint32_t *const s)
{
#pragma unroll 1
	for (uint32_t r = 0; r < 14; r++) {
		gbs_rc(&s[0], r);
		GBS_ROW(gbs_sbox_p, s, 0, 0)  GBS_ROW(gbs_sbox_p, s, 1, 1)  GBS_ROW(gbs_sbox_p, s, 2, 2)  GBS_ROW(gbs_sbox_p, s, 3, 3)
		GBS_ROW(gbs_sbox_p, s, 4, 4)  GBS_ROW(gbs_sbox_p, s, 5, 5)  GBS_ROW(gbs_sbox_p, s, 6, 6)  GBS_ROW(gbs_sbox_p, s, 7, 11)
		gbs_mix(s);
	}
}

__device__ __forceinline__ void gbs_perm_q(uint32_t *const s)
{
#pragma unroll 1
	for (uint32_t r = 0; r < 14; r++) {
		gbs_rc(&s[56], r);
		GBS_ROW(gbs_sbox_q, s, 0, 1)  GBS_ROW(gbs_sbox_q, s, 1, 3)  GBS_ROW(gbs_sbox_q, s, 2, 5)  GBS_ROW(gbs_sbox_q, s, 3, 11)
		GBS_ROW(gbs_sbox_q, s, 4, 0)  GBS_ROW(gbs_sbox_q, s, 5, 2)  GBS_ROW(gbs_sbox_q, s, 6, 4)  GBS_ROW(gbs_sbox_q, s, 7, 6)
		gbs_mix(s);
	}
}

#define GBS_IVW (6 * 8 + 1)	/* the IV's one byte, 0x02 at byte 126 */
#define GBS_IVM 0xC0000000u

/* One block from the IV and the output transform: s is the message on entry
 * and P(H) ^ H on exit. The registers hold one state, so the other one waits
 * in shared memory: st is GBS_ST_WORDS words, stride apart. */
__device__ __forceinline__ void gbs_compress(uint32_t *const s, uint32_t *const st, const uint32_t stride)
{
#pragma unroll
	for (int i = 0; i < 64; i++) st[i * stride] = s[i];
	s[GBS_IVW] ^= GBS_IVM;
	gbs_perm_p(s);
#pragma unroll
	for (int i = 0; i < 64; i++) { const uint32_t v = st[i * stride]; st[i * stride] = s[i]; s[i] = v; }
	gbs_perm_q(s);
#pragma unroll
	for (int i = 0; i < 64; i++) s[i] ^= st[i * stride];
	s[GBS_IVW] ^= GBS_IVM;
#pragma unroll
	for (int i = 0; i < 64; i++) st[i * stride] = s[i];
	gbs_perm_p(s);
#pragma unroll
	for (int i = 0; i < 64; i++) s[i] ^= st[i * stride];
}

#endif

#!/usr/bin/env python3
"""Emit cuda/upstream/quark/groestl512_bs_gen.h, the gate networks of the
bitsliced Groestl-512 kernel, after checking them against a reference.

  python3 scripts/gen-groestl-bitslice.py cuda/upstream/quark/groestl512_bs_gen.h

Layout. Two hashes a thread. s[row * 8 + plane] is one row of the 8x16 state:
bit 2c+h of it is bit `plane` of the byte at (row, column c) of hash h. One
row a word makes ShiftBytes a single rotate, which is the idea of the
two-hashes-a-thread kernel in tpfuemp's ccminer; the networks here are our own.

S-box. The Boyar-Peralta depth-16 circuit, its two linear layers rebuilt at
random and covered with 3-input LUTs (one lop3.b32 each), then shrunk by
resubstitution: 128 two-input gates became 74 LUTs. Complementing the inputs
costs nothing in a LUT, so Q's all-ones round constant is a second copy of the
same network instead of 64 NOTs a round.

MixBytes. circ(2,2,3,4,5,3,5,7) over the bit planes as 303 xor gates of two or
three inputs, from a greedy common-term search and the same resubstitution.

The search that found the tables is not kept: it is a randomised optimiser and
the tables are its output. What is kept is the proof that they are right.
"""
import random, struct, sys

# S-box: a gate is (inputs, truth table). Signals 0..7 are the inputs, x0 the
# low bit; gate k is signal 8+k. The table is lop3's: bit (a<<2 | b<<1 | c).
# MixBytes: a gate is the signals it xors. 0..63 are s[], gate k is 64+k.
SBOX = [
    ((5,6,0),150), ((7,8,2),90), ((1,9,8),150), ((1,2,3),150), ((11,10,5),90), ((4,6,12),150),
    ((1,7,12),190), ((1,8,14),166), ((13,15,9),90), ((7,16,4),90), ((6,17,11),72), ((9,18,13),156),
    ((16,19,8),90), ((20,16,4),150), ((10,18,21),90), ((1,7,12),40), ((8,12,7),24), ((23,24,19),150),
    ((7,12,25),150), ((20,26,0),90), ((12,25,27),90), ((22,19,27),160), ((0,21,29),106), ((13,30,19),150),
    ((28,31,16),90), ((17,32,29),150), ((20,10,33),106), ((9,34,17),90), ((35,28,18),106), ((36,34,15),24),
    ((36,34,31),150), ((36,15,26),150), ((36,15,31),40), ((39,38,40),72), ((39,37,40),156), ((41,42,31),90),
    ((26,43,37),224), ((37,38,44),180), ((39,38,44),96), ((9,45,10),72), ((46,47,26),90), ((43,48,17),40),
    ((48,49,42),90), ((45,50,43),150), ((35,51,49),106), ((10,42,52),106), ((22,50,53),106), ((20,42,54),106),
    ((27,50,55),106), ((51,56,28),160), ((8,45,57),106), ((51,58,50),90), ((22,59,35),72), ((8,12,43),40),
    ((61,47,52),150), ((48,53,0),160), ((27,63,50),108), ((12,59,64),106), ((60,62,65),105), ((42,64,45),90),
    ((16,67,60),106), ((68,58,62),105), ((59,58,12),108), ((17,21,43),40), ((71,64,68),150), ((21,48,72),106),
    ((53,70,73),150), ((57,56,70),150), ((65,75,61),150), ((67,51,32),40), ((9,67,49),106), ((77,72,78),105),
    ((77,70,75),105), ((77,70,54),150),
]
SBOX_OUT = [
    66, 69, 74, 76, 75, 79, 80, 81,
]
MIX = [
    (7,47), (64,32,40), (65,16,48), (15,23,63), (30,54,46), (30,62,38), (54,62,22), (68,69,70),
    (68,22), (71,72,62), (66,67,73), (74,56), (23,39,47), (0,16,40), (17,33,41), (31,47,64),
    (76,79,14), (77,80,70), (81,8,56), (78,81,82), (76,77,83), (7,15,55), (1,9,33), (14,31,63),
    (87,6,54), (46,39,88), (89,39,7), (76,8,32), (16,24,55), (91,92,56), (86,90,93), (49,57,94),
    (94,95,31), (85,96,73), (84,97), (48,55,63), (99,56,31), (50,58,34), (101,18,42), (9,17,57),
    (24,32,39), (1,41), (103,104,105), (100,102,106), (25,49,63), (108,57,51), (2,42), (64,110,15),
    (10,18,23), (112,33,59), (111,113,19), (58,35,43), (114,115,73), (109,116), (76,11,52), (118,43,60),
    (119,36,31), (120,101,73), (85,121,20), (122,26,44), (3,19,59), (123,124), (4,60), (126,51,31),
    (127,27,37), (44,45,55), (20,53,61), (35,12,20), (131,59,20), (130,132,63), (129,133,21), (128,134,39),
    (21,61), (52,60,62), (136,137,54), (36,5,13), (28,45,71), (138,139,140), (53,63), (6,30),
    (22,62,143), (144,31), (145,30,71), (38,46,146), (147,31,61), (76,148,46), (142,149,55), (37,14,22),
    (150,151,29), (40,48,23), (85,153,23), (154,0,23), (146,155,56), (156,24), (8,15,0), (158,48,32),
    (159,32,92), (146,105,57), (76,160,161), (162,108), (26,42,50), (164,9,2), (65,165,58), (1,17,39),
    (167,0,56), (166,168,108), (113,161,3), (85,43,51), (171,2,26), (172,50,27), (170,173), (19,27,55),
    (175,126,110), (176,34,58), (3,51,15), (118,28,63), (178,179,44), (177,146,180), (64,133,43), (4,45,28),
    (182,183,3), (184,5,29), (185,52,39), (44,21,29), (187,126,62), (139,68,6), (188,189,53), (88,64,30),
    (5,45,22), (192,37,55), (191,193,61), (159,56,23), (195,89), (101,2,10), (197,15,158), (103,154,47),
    (199,25,33), (198,200), (178,58,59), (112,202,105), (49,26,34), (139,12,28), (205,37,47), (206,130,60),
    (207,171,3), (208,11), (208,35,209), (204,210,9), (203,211,89), (132,36), (213,90,50), (118,176,10),
    (214,215), (13,37,52), (217,4,54), (14,38), (218,187,219), (220,148,12), (85,14,39), (142,69,192),
    (222,223,13), (85,87,47), (225,80,81), (226,106), (112,99,1), (64,111,16), (228,229,25), (230,83,58),
    (124,80,18), (204,110,11), (103,233,70), (232,234,43), (27,20,39), (236,3), (225,50,12), (237,238,44),
    (239,70,10), (115,18,19), (240,241,126), (11,19,44), (67,51,20), (244,183,139), (245,136,55), (243,246,59),
    (151,20,46), (248,137,21), (12,29,45), (249,250,5), (143,30,251), (67,71,13), (143,64,253), (254,136,53),
    (160,145,47), (256,39), (91,24,17), (108,144,1), (259,154,9), (258,260), (112,26,50), (77,40,9),
    (8,17,83), (264,79,108), (263,265,2), (262,266,24), (164,167,25), (10,34,47), (268,269,57), (178,145,11),
    (175,270,271), (144,58,27), (273,131,4), (172,179,18), (274,275), (124,205,23), (142,277,27), (187,278,79),
    (279,52), (183,151,53), (143,20,60), (281,282,13), (283,54,29), (219,68,5), (85,285,29), (286,136,23),
    (287,7,79), (143,219,63), (289,93), (87,38,23), (291,16,32), (292,200,56), (293,6,30), (158,104,79),
    (295,78,10), (204,57,18), (296,297,58), (76,175,289), (299,86,210), (300,102,25), (301,59), (172,282,28),
    (303,291,19), (213,304,269), (79,210,61), (237,217,15), (307,187,60), (306,308,36), (69,136,281), (310,12,36),
    (5,37,31), (29,54,39), (312,253,313), (314,62), (87,71), (92,316,0), (317,65), (264,167,316),
    (319,24,25), (320,155), (105,229,40), (322,297,25), (323,91), (64,165,3), (325,115,78), (175,326,316),
    (112,171,4), (328,34,59), (87,42,28), (71,36,44), (237,330,331), (329,332), (187,37,15), (334,119,19),
    (183,335,35), (336,5), (130,143,5), (338,250,331), (147,21,45), (340,52,217), (76,341,68), (64,342,47),
    (104,72,154), (344,8,63), (265,159,9), (76,346,72), (86,96,92), (348,164,269), (153,349), (210,41,39),
    (351,72,197), (352,17,27), (353,108,171), (213,79,72), (164,180,18), (355,356), (217,250,53), (358,43,23),
    (175,359,127), (360,36,47), (130,217,285), (362,28,44), (219,53,15), (364,47,55), (365,340,313),
]
MIX_OUT = [
    75, 98, 107, 117, 125, 135, 141, 152, 157, 163, 169, 174, 181, 186, 190, 194,
    196, 95, 201, 212, 216, 209, 221, 224, 82, 227, 231, 235, 242, 247, 252, 255,
    257, 261, 267, 272, 276, 280, 284, 288, 290, 294, 298, 302, 305, 309, 311, 315,
    318, 321, 324, 327, 333, 337, 339, 343, 345, 347, 350, 354, 357, 361, 363, 366,
]

M32 = 0xffffffff
SP = [0, 1, 2, 3, 4, 5, 6, 11]
SQ = [1, 3, 5, 11, 0, 2, 4, 6]


# ---------------------------------------------------------------- reference
def gmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        if a & 0x100:
            a ^= 0x11b
        b >>= 1
    return r


def aes_sbox():
    inv = [0] * 256
    for a in range(1, 256):
        for b in range(1, 256):
            if gmul(a, b) == 1:
                inv[a] = b
                break
    S = []
    for a in range(256):
        x = y = inv[a]
        for _ in range(4):
            x = ((x << 1) | (x >> 7)) & 0xff
            y ^= x
        S.append(y ^ 0x63)
    return S


AES = aes_sbox()


def _mixcol(st, i, j):
    v = 0
    for k, c in enumerate((2, 2, 3, 4, 5, 3, 5, 7)):
        v ^= gmul(c, st[(i + k) % 8][j])
    return v


def perm(st, isq):
    for r in range(14):
        if isq:
            st = [[v ^ 0xff for v in row] for row in st]
        for j in range(16):
            st[7 if isq else 0][j] ^= (j << 4) ^ r
        sh = SQ if isq else SP
        st = [[AES[st[i][(j + sh[i]) % 16]] for j in range(16)] for i in range(8)]
        st = [[_mixcol(st, i, j) for j in range(16)] for i in range(8)]
    return st


def to_st(b): return [[b[8 * j + i] for j in range(16)] for i in range(8)]
def from_st(st): return bytes(st[i][j] for j in range(16) for i in range(8))
def xs(a, b): return bytes(x ^ y for x, y in zip(a, b))


def groestl512(msg):
    msg = msg + b'\x80'
    msg += b'\0' * ((-len(msg) - 8) % 128)
    msg += struct.pack('>Q', (len(msg) + 8) // 128)
    h = b'\0' * 126 + b'\x02\x00'
    for o in range(0, len(msg), 128):
        m = msg[o:o + 128]
        h = xs(xs(from_st(perm(to_st(xs(h, m)), False)), from_st(perm(to_st(m), True))), h)
    return xs(from_st(perm(to_st(h), False)), h)[64:]


# ---------------------------------------------------------------- the model
def lop3(tt, a, b, c, mask=M32):
    r = 0
    for idx in range(8):
        if tt >> idx & 1:
            r |= (a if idx & 4 else ~a) & (b if idx & 2 else ~b) & (c if idx & 1 else ~c)
    return r & mask


def sbox_q():
    """The S-box of the complemented input: flip each LUT along its primary inputs."""
    out = []
    for ins, tt in SBOX:
        for k, i in enumerate(ins):
            if i < 8:
                m, sh = (0xF0, 0xCC, 0xAA)[k], 4 >> k
                tt = ((tt & m) >> sh) | ((tt & ~m & 0xff) << sh)
        out.append((ins, tt))
    return out


def run_sbox(gates, x, mask=M32):
    v = list(x)
    for (a, b, c), tt in gates:
        v.append(lop3(tt, v[a], v[b], v[c], mask))
    return [v[o] for o in SBOX_OUT]


def run_mix(s):
    v = list(s)
    for g in MIX:
        r = 0
        for i in g:
            r ^= v[i]
        v.append(r)
    return [v[o] for o in MIX_OUT]


def bperm(x, y, sel):
    v = x | (y << 32)
    return sum(((v >> (8 * ((sel >> (4 * i)) & 7))) & 0xff) << (8 * i) for i in range(4))


def transpose(d):
    for m, n, prs in ((0xaaaaaaaa, 1, ((0, 1), (2, 3), (4, 5), (6, 7))),
                      (0xcccccccc, 2, ((0, 2), (1, 3), (4, 6), (5, 7))),
                      (0xf0f0f0f0, 4, ((0, 4), (1, 5), (2, 6), (3, 7)))):
        for i, j in prs:
            t = (d[i] ^ (d[j] << n)) & m
            d[i] ^= t
            d[j] ^= t >> n


def load(blocks):
    """Two 128-byte blocks as 32 LE words each, as gbs_load does it."""
    s = []
    for r in range(8):
        rb, rw = r & 3, r >> 2
        sel = rb | ((4 + rb) << 4)
        W = []
        for k in range(8):
            b, cc = blocks[k & 1], k >> 1
            lo = bperm(b[2 * cc + rw], b[8 + 2 * cc + rw], sel)
            hi = bperm(b[16 + 2 * cc + rw], b[24 + 2 * cc + rw], sel)
            W.append(bperm(lo, hi, 0x5410))
        transpose(W)
        s += W
    return s


def store(s):
    s = list(s)
    out = [[0] * 16, [0] * 16]
    for r in range(8):
        W = s[r * 8:r * 8 + 8]
        transpose(W)
        s[r * 8:r * 8 + 8] = W
    for h in range(2):
        for w in range(16):
            c = 8 + (w >> 1)
            k, n, r0 = 2 * (c & 3) + h, c >> 2, 4 * (w & 1)
            sel = n | ((4 + n) << 4)
            lo = bperm(s[r0 * 8 + k], s[(r0 + 1) * 8 + k], sel)
            hi = bperm(s[(r0 + 2) * 8 + k], s[(r0 + 3) * 8 + k], sel)
            out[h][w] = bperm(lo, hi, 0x5410)
    return out


RCM = (0xCCCCCCCC, 0xF0F0F0F0, 0xFF00FF00, 0xFFFF0000)
SQG = sbox_q()


def rnd(s, r, isq):
    row = 56 if isq else 0
    for p in range(4):
        if r >> p & 1:
            s[row + p] ^= M32
        s[row + 4 + p] ^= RCM[p]
    sh = SQ if isq else SP
    for i in range(8):
        y = run_sbox(SQG if isq else SBOX, s[i * 8:i * 8 + 8])
        n = 2 * sh[i]
        s[i * 8:i * 8 + 8] = [((v >> n) | (v << (32 - n))) & M32 for v in y]
    s[:] = run_mix(s)


IVW, IVM = 6 * 8 + 1, 0xC0000000


def hash2(m0, m1):
    """Groestl-512 of two messages of up to 119 bytes, through the bitsliced model."""
    blocks = []
    for m in (m0, m1):
        b = m + b'\x80' + b'\0' * (126 - len(m)) + b'\x01'
        blocks.append(struct.unpack('<32I', b))
    s = load(blocks)
    st = list(s)
    s[IVW] ^= IVM
    for r in range(14):
        rnd(s, r, False)
    s, st = st, s
    for r in range(14):
        rnd(s, r, True)
    s = [a ^ b for a, b in zip(s, st)]
    s[IVW] ^= IVM
    st = list(s)
    for r in range(14):
        rnd(s, r, False)
    o = store([a ^ b for a, b in zip(s, st)])
    return struct.pack('<16I', *o[0]), struct.pack('<16I', *o[1])


def check():
    assert groestl512(b'abc').hex().startswith('70e1c68c60df3b655339d67dc291cc3f1dde4ef343f11b23fdd44957693815a7')
    planes = [sum(((x >> b) & 1) << x for x in range(256)) for b in range(8)]
    full = (1 << 256) - 1
    for gates, neg in ((SBOX, 0), (SQG, 0xff)):
        y = run_sbox(gates, planes, full)
        for x in range(256):
            assert sum(((y[b] >> x) & 1) << b for b in range(8)) == AES[x ^ neg]
    rng = random.Random(1)
    for n in (64, 64, 80, 0, 119):
        a = bytes(rng.randrange(256) for _ in range(n))
        b = bytes(rng.randrange(256) for _ in range(n))
        assert hash2(a, b) == (groestl512(a), groestl512(b)), 'the bitsliced model disagrees with Groestl-512'


# ---------------------------------------------------------------- emission
def emit_sbox(name, gates):
    L = ['__device__ __forceinline__ void %s(uint32_t &x0, uint32_t &x1, uint32_t &x2, uint32_t &x3,' % name,
         '\tuint32_t &x4, uint32_t &x5, uint32_t &x6, uint32_t &x7)', '{']
    nm = lambda i: 'x%d' % i if i < 8 else 't%d' % i
    L.append('\tuint32_t ' + ', '.join(nm(8 + k) for k in range(len(gates))) + ';')
    for k, ((a, b, c), tt) in enumerate(gates):
        L.append('\tGBS_L3(%s, %s, %s, %s, 0x%02X);' % (nm(8 + k), nm(a), nm(b), nm(c), tt))
    for i, o in enumerate(SBOX_OUT):
        L.append('\tx%d = %s;' % (i, nm(o)))
    return L + ['}']


def emit_mix():
    nm = lambda i: 's[%d]' % i if i < 64 else 'g%d' % i
    L = ['__device__ __forceinline__ void gbs_mix(uint32_t *const s)', '{']
    L.append('\tuint32_t ' + ', '.join(nm(64 + k) for k in range(len(MIX))) + ';')
    for k, g in enumerate(MIX):
        if len(g) == 3:
            L.append('\tGBS_L3(%s, %s, %s, %s, 0x96);' % (nm(64 + k), nm(g[0]), nm(g[1]), nm(g[2])))
        else:
            L.append('\t%s = %s ^ %s;' % (nm(64 + k), nm(g[0]), nm(g[1])))
    for i, o in enumerate(MIX_OUT):
        L.append('\ts[%d] = %s;' % (i, nm(o)))
    return L + ['}']


def main():
    check()
    L = ['/* Generated by scripts/gen-groestl-bitslice.py: do not edit.',
         ' *',
         ' * Gate networks of the bitsliced Groestl-512 kernel, one lop3 a gate:',
         ' * S-box %d gates, MixBytes %d. s[row * 8 + plane]; see the generator. */' % (len(SBOX), len(MIX)),
         '#ifndef GROESTL512_BS_GEN_H', '#define GROESTL512_BS_GEN_H', '',
         '#if AVIAN_NO_PTX',
         '#define GBS_L3(d, a, b, c, tt) d = avian_lop3(a, b, c, tt)',
         '#else',
         '#define GBS_L3(d, a, b, c, tt) asm("lop3.b32 %0, %1, %2, %3, " #tt ";" : "=r"(d) : "r"(a), "r"(b), "r"(c))',
         '#endif', '',
         '/* AES S-box on eight bit planes, x0 the low bit. */']
    L += emit_sbox('gbs_sbox_p', SBOX) + ['', "/* The S-box of the complemented input: Q's all-ones constant for free. */"]
    L += emit_sbox('gbs_sbox_q', SQG) + ['', '/* MixBytes, in place. */']
    L += emit_mix() + ['', '#endif']
    text = '\n'.join(L) + '\n'
    if len(sys.argv) > 1:
        open(sys.argv[1], 'w', newline='\n').write(text)
        print('checked and wrote %s: S-box %d gates, MixBytes %d' % (sys.argv[1], len(SBOX), len(MIX)))
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()

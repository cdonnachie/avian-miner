
if HAVE_APPLE
# MacOS uses Homebrew to install needed packages but they aren't linked for
# the jansson test in configure. Ignore the failed test & link them now,
# different path for different CPU arch.

if ARCH_ARM64
  EXTRA_INCLUDES = -I/opt/homebrew/include
  EXTRA_LIBS     = -L/opt/homebrew/lib
else
  EXTRA_INCLUDES = -I/usr/local/include
  EXTRA_LIBS     = -L/usr/local/lib
endif

else

if WANT_JANSSON
# Can't find jansson libraries, compile the included source code.
  EXTRA_INCLUDES = -I$(top_srcdir)/compat/jansson
  EXTRA_LIBS     = -L$(top_srcdir)/compat/jansson
else
  EXTRA_INCLUDES =
  EXTRA_LIBS     =
endif

endif

EXTRA_DIST = example-cfg.json nomacro.pl

SUFFIXES = .cu

SUBDIRS = compat

ALL_INCLUDES = @PTHREAD_FLAGS@ -fno-strict-aliasing $(EXTRA_INCLUDES) -I. @AVIAN_SV2_CFLAGS@

bin_PROGRAMS = avian-miner

dist_man_MANS = cpuminer.1

avian_miner_SOURCES = \
  dummy.cpp \
  cpu-miner.c \
  util.c \
  api.c \
  sysinfos.c \
  algo-gate-api.c\
  malloc-huge.c \
  simd-utils/simd-constants.c \
  algo/argon2d/argon2d-gate.c \
  algo/argon2d/blake2/blake2b.c \
  algo/argon2d/argon2d/argon2.c \
  algo/argon2d/argon2d/core.c \
  algo/argon2d/argon2d/opt.c \
  algo/argon2d/argon2d/argon2d_thread.c \
  algo/argon2d/argon2d/encoding.c \
  algo/blake/sph_blake.c \
  algo/blake/blake256-hash.c \
  algo/blake/blake512-hash.c \
  algo/blake/blake-gate.c \
  algo/blake/blake.c \
  algo/blake/blake-4way.c \
  algo/blake/sph_blake2b.c \
  algo/blake/sph-blake2s.c \
  algo/blake/blake2s-hash.c \
  algo/blake/blake2s.c \
  algo/blake/blake2b-hash.c \
  algo/blake/blake2b.c \
  algo/blake/blakecoin-gate.c \
  algo/blake/mod_blakecoin.c \
  algo/blake/blakecoin.c \
  algo/blake/blakecoin-4way.c \
  algo/blake/pentablake-gate.c \
  algo/blake/pentablake-4way.c \
  algo/blake/pentablake.c \
  algo/bmw/sph_bmw.c \
  algo/bmw/bmw256-hash-4way.c \
  algo/bmw/bmw512-hash-4way.c \
  algo/bmw/bmw256.c \
  algo/bmw/bmw512-gate.c \
  algo/bmw/bmw512.c \
  algo/bmw/bmw512-4way.c \
  algo/cubehash/cubehash_sse2.c\
  algo/cubehash/cube-hash-2way.c \
  algo/cubehash/sph_cubehash.c \
  algo/echo/sph_echo.c \
  algo/echo/echo-hash-4way.c \
  algo/echo/aes_ni/hash.c\
  algo/gost/sph_gost.c \
  algo/groestl/groestl-gate.c \
  algo/groestl/groestl512-hash-4way.c \
  algo/groestl/groestl256-hash-4way.c \
  algo/groestl/sph_groestl.c \
  algo/groestl/groestl.c \
  algo/groestl/groestl-4way.c \
  algo/groestl/myrgr-gate.c \
  algo/groestl/myrgr-4way.c \
  algo/groestl/myr-groestl.c \
  algo/groestl/aes_ni/hash-groestl.c \
  algo/groestl/aes_ni/hash-groestl256.c \
  algo/fugue/sph_fugue.c \
  algo/fugue/fugue-aesni.c \
  algo/hamsi/sph_hamsi.c \
  algo/hamsi/hamsi-hash-4way.c \
  algo/haval/haval.c \
  algo/haval/haval-hash-4way.c \
  algo/jh/sph_jh.c \
  algo/jh/jh-hash-4way.c \
  algo/jh/jha-gate.c \
  algo/jh/jha-4way.c \
  algo/jh/jha.c \
  algo/keccak/sph_keccak.c \
  algo/keccak/keccak.c\
  algo/keccak/keccak-hash-4way.c \
  algo/keccak/keccak-4way.c\
  algo/keccak/keccak-gate.c \
  algo/keccak/sha3d-4way.c \
  algo/keccak/sha3d.c \
  algo/lanehash/lane.c \
  algo/luffa/luffa_for_sse2.c \
  algo/luffa/luffa-hash-2way.c \
  algo/luffa/sph_luffa.c \
  algo/lyra2/lyra2.c \
  algo/lyra2/sponge.c \
  algo/lyra2/sponge-2way.c \
  algo/lyra2/lyra2-hash-2way.c \
  algo/lyra2/lyra2-gate.c \
  algo/lyra2/lyra2rev2.c \
  algo/lyra2/lyra2rev2-4way.c \
  algo/lyra2/lyra2rev3.c \
  algo/lyra2/lyra2rev3-4way.c \
  algo/lyra2/lyra2re.c \
  algo/lyra2/lyra2z-4way.c \
  algo/lyra2/lyra2z330.c \
  algo/lyra2/lyra2h.c \
  algo/lyra2/lyra2h-4way.c \
  algo/lyra2/allium-4way.c \
  algo/lyra2/phi2-4way.c \
  algo/lyra2/phi2.c \
  algo/m7m/m7m.c \
  algo/nist5/nist5-gate.c \
  algo/nist5/nist5-4way.c \
  algo/nist5/nist5.c \
  algo/nist5/zr5.c \
  algo/panama/panama-hash-4way.c \
  algo/panama/sph_panama.c \
  algo/quark/quark-gate.c \
  algo/quark/quark.c \
  algo/quark/quark-4way.c \
  algo/quark/anime-gate.c \
  algo/quark/anime.c \
  algo/quark/anime-4way.c \
  algo/quark/hmq1725-gate.c \
  algo/quark/hmq1725-4way.c \
  algo/quark/hmq1725.c \
  algo/qubit/qubit-gate.c \
  algo/qubit/qubit.c \
  algo/qubit/qubit-2way.c \
  algo/qubit/deep-gate.c \
  algo/qubit/deep-2way.c \
  algo/qubit/deep.c \
  algo/ripemd/sph_ripemd.c \
  algo/ripemd/ripemd-hash-4way.c \
  algo/ripemd/lbry-gate.c \
  algo/ripemd/lbry.c \
  algo/ripemd/lbry-4way.c \
  algo/scrypt/scrypt.c \
  algo/scrypt/scrypt-core-4way.c \
  algo/scrypt/neoscrypt.c \
  algo/sha/sha1.c \
  algo/sha/sha1-hash.c \
  algo/sha/sha256-hash.c \
  algo/sha/sph_sha2.c \
  algo/sha/sph_sha2big.c \
  algo/sha/sha256-hash-4way.c \
  algo/sha/sha512-hash-4way.c \
  algo/sha/hmac-sha256-hash.c \
  algo/sha/hmac-sha256-hash-4way.c \
  algo/sha/sha256d.c \
  algo/sha/sha256d-4way.c \
  algo/sha/sha256t-gate.c \
  algo/sha/sha256t-4way.c \
  algo/sha/sha256q-4way.c \
  algo/sha/sha256q.c \
  algo/sha/sha512256d-4way.c \
  algo/sha/sha256dt.c \
  algo/shabal/sph_shabal.c \
  algo/shabal/shabal-hash-4way.c \
  algo/shavite/sph_shavite.c \
  algo/shavite/sph-shavite-aesni.c \
  algo/shavite/shavite-hash-2way.c \
  algo/shavite/shavite-hash-4way.c \
  algo/simd/sph_simd.c \
  algo/simd/simd-hash-2way.c \
  algo/skein/sph_skein.c \
  algo/skein/skein-hash-4way.c \
  algo/skein/skein.c \
  algo/skein/skein-4way.c \
  algo/skein/skein-gate.c \
  algo/skein/skein2.c \
  algo/skein/skein2-4way.c \
  algo/sm3/sm3.c \
  algo/sm3/sm3-hash-4way.c \
  algo/swifftx/swifftx.c \
  algo/tiger/sph_tiger.c \
  algo/verthash/verthash-gate.c \
  algo/verthash/Verthash.c \
  algo/verthash/fopen_utf8.c \
  algo/verthash/tiny_sha3/sha3.c \
  algo/verthash/tiny_sha3/sha3-4way.c \
  algo/whirlpool/sph_whirlpool.c \
  algo/whirlpool/whirlpool-gate.c \
  algo/whirlpool/whirlpool.c \
  algo/whirlpool/whirlpoolx.c \
  algo/x11/x11-gate.c \
  algo/x11/x11.c \
  algo/x11/x11-4way.c \
  algo/x11/x11gost-gate.c \
  algo/x11/x11gost.c \
  algo/x11/x11gost-4way.c \
  algo/x11/c11-gate.c \
  algo/x11/c11.c \
  algo/x11/c11-4way.c \
  algo/x11/tribus-gate.c \
  algo/x11/tribus.c \
  algo/x11/tribus-4way.c \
  algo/x11/timetravel-gate.c \
  algo/x11/timetravel.c \
  algo/x11/timetravel-4way.c \
  algo/x11/timetravel10-gate.c \
  algo/x11/timetravel10.c \
  algo/x11/timetravel10-4way.c \
  algo/x11/x11evo.c \
  algo/x11/x11evo-4way.c \
  algo/x11/x11evo-gate.c \
  algo/x12/x12-gate.c \
  algo/x12/x12.c \
  algo/x12/x12-4way.c \
  algo/x13/x13-gate.c \
  algo/x13/x13.c \
  algo/x13/x13-4way.c \
  algo/x13/x13sm3-gate.c \
  algo/x13/x13sm3.c \
  algo/x13/x13sm3-4way.c \
  algo/x13/phi1612-gate.c \
  algo/x13/phi1612.c \
  algo/x13/phi1612-4way.c \
  algo/x13/skunk-gate.c \
  algo/x13/skunk-4way.c \
  algo/x13/skunk.c \
  algo/x13/x13bcd-4way.c \
  algo/x13/x13bcd.c \
  algo/x14/x14-gate.c \
  algo/x14/x14.c \
  algo/x14/x14-4way.c \
  algo/x14/veltor-gate.c \
  algo/x14/veltor.c \
  algo/x14/veltor-4way.c \
  algo/x14/polytimos-gate.c \
  algo/x14/polytimos.c \
  algo/x14/polytimos-4way.c \
  algo/x14/axiom.c \
  algo/x15/x15-gate.c \
  algo/x15/x15.c \
  algo/x15/x15-4way.c \
  algo/x16/x16r-gate.c \
  algo/x16/x16r.c \
  algo/x16/x16r-4way.c \
  algo/x16/x16rv2.c \
  algo/x16/x16rv2-4way.c \
  algo/x16/x16rt.c \
  algo/x16/x16rt-4way.c \
  algo/x16/hex.c \
  algo/x16/x20r.c \
  algo/x16/x21s-4way.c \
  algo/x16/x21s.c \
  algo/x16/minotaur.c \
  algo/x17/x17-gate.c \
  algo/x17/x17.c \
  algo/x17/x17-4way.c \
  algo/x17/xevan-gate.c \
  algo/x17/xevan.c \
  algo/x17/xevan-4way.c \
  algo/x17/sonoa-gate.c \
  algo/x17/sonoa-4way.c \
  algo/x17/sonoa.c \
  algo/x22/x22i-4way.c \
  algo/x22/x22i.c \
  algo/x22/x22i-gate.c \
  algo/x22/x25x.c \
  algo/x22/x25x-4way.c \
  algo/yespower/yespower-gate.c \
  algo/yespower/yespower-blake2b.c \
  algo/yespower/crypto/hmac-blake2b.c \
  algo/yespower/yescrypt-r8g.c \
  algo/yespower/yespower-opt.c \
  algo/yespower/yespower-ref.c \
  algo/yespower/yespower-blake2b-ref.c

if HAVE_WINDOWS
   avian_miner_SOURCES += compat/winansi.c
endif

if USE_ASM
   disable_flags =
   avian_miner_SOURCES += asm/neoscrypt_asm.S
else
   disable_flags = -DNOASM
endif

avian_miner_LDFLAGS = @LDFLAGS@
if HAVE_AVIAN_SV2
  avian_miner_SOURCES += avian/sv2.c avian/selftest.c avian/ntime.c
endif

avian_miner_LDADD	= $(EXTRA_LIBS) @LIBCURL@ -ljansson @PTHREAD_LIBS@ @WS2_LIBS@ -lgmp @AVIAN_SV2_LIBS@

if HAVE_WINDOWS
# No libdl on Windows: dynamic loading there is LoadLibrary. These are what
# the Rust static library's std needs, and they come after it because ld
# resolves static archives in the order given, so the -lws2_32 earlier on
# the line has already been passed over by the time it is asked for.
avian_miner_LDADD += -lws2_32 -lbcrypt -luserenv -lntdll -ladvapi32 -lsecur32
else
# Rust's std uses dlopen on Linux.
avian_miner_LDADD += -ldl
endif
avian_miner_CPPFLAGS = @LIBCURL_CPPFLAGS@ $(ALL_INCLUDES)
avian_miner_CFLAGS = -Wno-pointer-sign -Wno-pointer-to-int-cast $(disable_flags)

if ARCH_ARM64
   avian_miner_CFLAGS += -flax-vector-conversions
endif

if HAVE_WINDOWS

# use to profile an object
# gprof_cflags = -pg -g3
# avian_miner_LDFLAGS += -pg
# avian_miner_CFLAGS += -fno-inline-functions -static

# copy/paste from generated Makefile
common_ccflags = $(DEFS) $(ALL_INCLUDES) $(avian_miner_CPPFLAGS) $(CPPFLAGS) $(avian_miner_CFLAGS) $(CFLAGS)

# special CFLAGS (if you find a simpler way to do that tell me ;)
avian_miner-neoscrypt.o: neoscrypt.c
	@echo "CUSTOM ${@}: ${filter %.o,${^}} ${filter %.c,${^}}"
	$(CC) $(common_ccflags) -g -O3 $(gprof_cflags) -MT $@ -MD -MP -c -o $@ $<

endif

if HAVE_CUDA
# automake has no notion of .cu, so these are compiled by an explicit rule and
# handed to the link as objects. Listed one per line rather than globbed, so
# that what is built is what is in the tree and `make dist` sees them.
CUDA_SOURCES = \
  cuda/device.cu \
  cuda/avian-cuda-support.cu \
  algo/x16/x16rt-cuda-batch.cu \
  cuda/upstream/JHA/cuda_jha_keccak512.cu \
  cuda/upstream/cuda_checkhash.cu \
  cuda/upstream/quark/cuda_bmw512.cu \
  cuda/upstream/quark/cuda_jh512.cu \
  cuda/upstream/quark/cuda_quark_blake512.cu \
  cuda/upstream/quark/cuda_quark_groestl512.cu \
  cuda/upstream/quark/cuda_quark_keccak512.cu \
  cuda/upstream/quark/cuda_skein512.cu \
  cuda/upstream/qubit/qubit_luffa512_alexis.cu \
  cuda/upstream/x11/cuda_x11_cubehash512.cu \
  cuda/upstream/x11/cuda_x11_cubehash512_80.cu \
  cuda/upstream/x11/cuda_x11_echo_sp.cu \
  cuda/upstream/x11/cuda_x11_luffa512.cu \
  cuda/upstream/x11/cuda_x11_shavite512.cu \
  cuda/upstream/x11/cuda_x11_shavite512_sp.cu \
  cuda/upstream/x11/cuda_x11_simd512.cu \
  cuda/upstream/x13/cuda_x13_fugue512_sp.cu \
  cuda/upstream/x13/cuda_x13_hamsi512.cu \
  cuda/upstream/x13/cuda_x13_hamsi512_sp.cu \
  cuda/upstream/x15/cuda_x14_shabal512.cu \
  cuda/upstream/x15/cuda_x14_shabal512_sp.cu \
  cuda/upstream/x15/cuda_x15_whirlpool.cu \
  cuda/upstream/x16r/cuda_x16_echo512.cu \
  cuda/upstream/x16r/cuda_x16_fugue512.cu \
  cuda/upstream/x16r/cuda_x16_shabal512.cu \
  cuda/upstream/x16r/cuda_x16_shavite512.cu \
  cuda/upstream/x16r/cuda_x16_simd512_80.cu \
  cuda/upstream/x16r/cuda_x16_whirlpool.cu \
  cuda/upstream/x17/cuda_x17_sha512.cu

CUDA_OBJECTS = $(CUDA_SOURCES:.cu=.o)

# -I. and -I$(top_srcdir) find cpuminer-config.h, miner.h and compat.h;
# cuda/ finds the shim; cuda/upstream/ finds the vendored headers at the paths
# upstream's own includes expect.
GPU_INCLUDES = -I. -I$(top_srcdir) -I$(top_srcdir)/cuda -I$(top_srcdir)/cuda/upstream

if HAVE_HIP
# Ahead of everything else, so a kernel's #include <cuda_runtime.h> finds the
# rename shim rather than a CUDA toolkit that may also be installed. An Nvidia
# build never sees this directory.
GPU_SHIM_INCLUDES = -I$(top_srcdir)/cuda/hip
else
GPU_SHIM_INCLUDES =
endif

# -w because the vendored kernels are not ours to make warning clean, and the
# noise would bury warnings from code that is.
#
# $(EXTRA_INCLUDES) because the kernels' host half includes miner.h, which
# includes jansson.h, and on a machine with no system jansson that header is
# the bundled one under compat/. The C sources reach it through ALL_INCLUDES
# and this rule needs the same. A build host that has jansson installed never
# notices the omission, which is why it survived until the first build on one
# that does not.
GPU_FLAGS = @GPUCC_ARCH@ -std=c++14 -w $(GPU_SHIM_INCLUDES) $(GPU_INCLUDES) $(EXTRA_INCLUDES)

.cu.o:
	@GPUCC@ $(GPU_FLAGS) -c -o $@ $<

avian_miner_LDADD += $(CUDA_OBJECTS) @GPU_LDFLAGS@ @GPU_LIBS@
avian_miner_SOURCES += algo/x16/x16rt-cuda-gate.c                        algo/x16/x16rt-cuda-host.c
EXTRA_DIST = $(CUDA_SOURCES)
CLEANFILES = $(CUDA_OBJECTS)
endif

#include "x16r-gate.h"
#include "algo/sha/sha256d.h"

__thread char x16r_hash_order[ X16R_HASH_FUNC_COUNT + 1 ] = { 0 };

void (*x16_r_s_getAlgoString) ( const uint8_t*, char* ) = NULL;

#if defined(X16R_8WAY)

__thread x16r_8way_context_overlay x16r_ctx;

#elif defined(X16R_4WAY)

__thread x16r_4way_context_overlay x16r_ctx;

#elif defined(X16R_2WAY)

__thread x16r_2x64_context_overlay x16r_ctx;

#endif

__thread x16r_context_overlay x16r_ref_ctx;

////////////////
//
//   X16RT

void x16rt_getTimeHash( const uint32_t timeStamp, void* timeHash )
{
    int32_t maskedTime = timeStamp & 0xffffff80;
    sha256d( (unsigned char*)timeHash, (const unsigned char*)( &maskedTime ),
             sizeof( maskedTime ) );
}

void x16rt_getAlgoString( const uint32_t *timeHash, char *output)
{
   char *sptr = output;
   uint8_t* data = (uint8_t*)timeHash;

   for (uint8_t j = 0; j < X16R_HASH_FUNC_COUNT; j++) {
      uint8_t b = (15 - j) >> 1; // 16 ascii hex chars, reversed
      uint8_t algoDigit = (j & 1) ? data[b] & 0xF : data[b] >> 4;

      if (algoDigit >= 10)
         sprintf(sptr, "%c", 'A' + (algoDigit - 10));
      else
         sprintf(sptr, "%u", (uint32_t) algoDigit);
      sptr++;
   }
   *sptr = '\0';
}

/* The sixteen functions in enum order, abbreviated the way T-Rex abbreviates
 * them so that someone watching a GPU and a CPU miner side by side reads the
 * same names in both. */
static const char *const x16r_algo_names[ X16R_HASH_FUNC_COUNT ] =
{
   "Blk", "Bmw", "Gro", "Jh5", "Kck", "Ske", "Luf", "Cub",
   "Shv", "Sim", "Ech", "Ham", "Fug", "Shb", "Wrl", "Sha"
};

/* What each one costs on this build's CPU path, which is the whole point of
 * the colour: one glance says whether this epoch's order suits the machine.
 * 0 cheap, 1 middling, 2 expensive.
 *
 * SIMD and Hamsi are the slow pair on every CPU. Groestl, Shavite, Echo and
 * Fugue have AES-NI implementations here and drop a tier when the build has
 * them, which is a large enough difference to be worth tracking rather than
 * hard-coding one answer for both builds.
 *
 * These are CPU costs, so the colours will not always agree with a GPU
 * miner's. JH is the clearest case: cheap here, middling there. */
#if defined(__AES__) || defined(__ARM_FEATURE_AES)
  #define X16R_AES_COST 1
#else
  #define X16R_AES_COST 2
#endif

static const uint8_t x16r_algo_cost[ X16R_HASH_FUNC_COUNT ] =
{
   0,              // BLAKE
   0,              // BMW
   X16R_AES_COST,  // GROESTL
   0,              // JH
   0,              // KECCAK
   0,              // SKEIN
   1,              // LUFFA
   1,              // CUBEHASH
   X16R_AES_COST,  // SHAVITE
   2,              // SIMD
   X16R_AES_COST,  // ECHO
   2,              // HAMSI
   X16R_AES_COST,  // FUGUE
   0,              // SHABAL
   1,              // WHIRLPOOL
   0               // SHA_512
};

void x16rt_log_hash_order( const char *order, const uint32_t ntime )
{
   char line[ X16R_HASH_FUNC_COUNT * 20 + 1 ];
   int len = 0;

   for ( int i = 0; i < X16R_HASH_FUNC_COUNT; i++ )
   {
      const char c = order[i];
      const int algo = ( c >= 'A' ) ? ( c - 'A' + 10 ) : ( c - '0' );
      const char *colour = "";
      const char *reset = "";

      if ( algo < 0 || algo >= X16R_HASH_FUNC_COUNT ) continue;
      if ( use_colors )
      {
         switch ( x16r_algo_cost[ algo ] )
         {
            case 0:  colour = CL_GRN; break;
            case 2:  colour = CL_RED; break;
            default: colour = CL_YLW; break;
         }
         reset = CL_N;
      }
      len += snprintf( line + len, sizeof( line ) - len, "%s%s%s",
                       colour, x16r_algo_names[ algo ], reset );
      if ( len >= (int)sizeof( line ) ) break;
   }

   applog( LOG_INFO, "Hash order %s, Ntime %08x", order, ntime );
   applog( LOG_INFO, "%s", line );
}

bool register_x16rt_algo( algo_gate_t* gate )
{
#if defined(HAVE_CUDA)
  /* --gpu replaces the CPU scan rather than adding to it. One algorithm runs
   * per process here, so a GPU run is wholly a GPU run, and the CPU is left
   * for the MinotaurX process alongside it. */
  if ( opt_gpu )
     return register_x16rt_cuda_algo( gate );
#endif
#if defined(X16RT_8WAY)
  gate->scanhash  = (void*)&scanhash_x16rt_8way;
  gate->hash      = (void*)&x16r_8way_hash;
#elif defined(X16RT_4WAY)
  gate->scanhash  = (void*)&scanhash_x16rt_4way;
  gate->hash      = (void*)&x16r_4way_hash;
#elif defined(X16RT_2WAY)
  gate->scanhash  = (void*)&scanhash_x16rt_2x64;
  gate->hash      = (void*)&x16r_2x64_hash;
#else
  gate->scanhash  = (void*)&scanhash_x16rt;
  gate->hash      = (void*)&x16r_hash;
#endif
  gate->optimizations = SSE2_OPT | AES_OPT | AVX2_OPT | AVX512_OPT | VAES_OPT
                      | NEON_OPT;
  opt_target_factor = 256.0;
  return true;
};

////////////////////
//
//    HEX

///////////////////////////////
//
//   X21S

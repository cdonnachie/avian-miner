/* Loading the vendor backends and routing a batch to the right one.
 *
 * See cuda/avian-gpu-backend.h for why they are loaded rather than imported.
 * This file is plain host C: it names no GPU runtime symbol, which is what
 * lets one binary serve a machine with either vendor or both, and is why it
 * compiles with the C compiler rather than nvcc or hipcc.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cuda/avian-gpu-backend.h"
#include "miner.h"

#ifdef _WIN32
#  include <windows.h>
#  define AVIAN_GPU_DLOPEN( path )        ( (void *) LoadLibraryA( path ) )
#  define AVIAN_GPU_DLSYM( h, name )      ( (void *) GetProcAddress( (HMODULE) (h), name ) )
#  define AVIAN_GPU_DLCLOSE( h )          FreeLibrary( (HMODULE) (h) )
#  define AVIAN_GPU_SEP                   '\\'
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  define AVIAN_GPU_DLOPEN( path )        dlopen( path, RTLD_NOW | RTLD_LOCAL )
#  define AVIAN_GPU_DLSYM( h, name )      dlsym( h, name )
#  define AVIAN_GPU_DLCLOSE( h )          dlclose( h )
#  define AVIAN_GPU_SEP                   '/'
#endif

/* RTLD_LOCAL above is not incidental. Both libraries define the same
 * avian_cuda_* names, and loading the second into the global namespace would
 * leave whichever loaded first answering for both. Keeping each one's symbols
 * to itself is what makes two tables of function pointers mean two
 * implementations. */

#define AVIAN_GPU_MAX_DEVICES 32

typedef int         (*avian_gpu_scan_fn)( const avian_cuda_job *, uint32_t *, int, uint32_t * );
typedef int         (*avian_gpu_count_fn)( void );
typedef const char *(*avian_gpu_name_fn)( int );
typedef void        (*avian_gpu_logger_fn)( avian_cuda_log_fn );
typedef void        (*avian_gpu_diag_fn)( avian_cuda_hash_n_fn, avian_cuda_prehash_fn );
typedef int         (*avian_gpu_abi_fn)( void );

struct backend
{
   const char          *vendor;      /* "Nvidia" or "AMD", for logs */
   const char          *library;     /* the file name we tried */
   void                *handle;      /* NULL when absent, which is normal */
   bool                 found;       /* the file was on disk, loaded or not */
   avian_gpu_scan_fn    scan_batch;
   avian_gpu_count_fn   device_count;
   avian_gpu_name_fn    device_name;
   avian_gpu_logger_fn  set_logger;
   avian_gpu_diag_fn    set_diagnostics;
   int                  devices;
};

static struct backend s_backend[2] = {
   { .vendor = "Nvidia", .library = AVIAN_GPU_NVIDIA_LIB_NAME },
   { .vendor = "AMD",    .library = AVIAN_GPU_AMD_LIB_NAME    },
};

/* The flat numbering the miner sees. */
static struct
{
   struct backend *backend;
   int             local;            /* that backend's own device index */
   int             shown;            /* its number before --devices narrowed
                                      * the list, so a log line still names
                                      * the card the operator asked for */
} s_device[AVIAN_GPU_MAX_DEVICES];

static int s_devices = 0;
static bool s_initialised = false;

/* The directory holding this executable, because that is where `make` and
 * every package put the backends, and dlopen on a bare name does not look
 * there. Windows' loader does search the application directory, but asking
 * for the same path on both keeps one code path tested twice.
 *
 * Returns false if it cannot be determined, which leaves the bare name as
 * the only attempt rather than failing outright. */
static bool exe_directory( char *out, size_t len )
{
   char path[1024];
   char *slash;

#ifdef _WIN32
   DWORD n = GetModuleFileNameA( NULL, path, (DWORD) sizeof(path) );
   if ( n == 0 || n >= sizeof(path) )
      return false;
#else
   ssize_t n = readlink( "/proc/self/exe", path, sizeof(path) - 1 );
   if ( n <= 0 || (size_t) n >= sizeof(path) )
      return false;
   path[n] = '\0';
#endif

   slash = strrchr( path, AVIAN_GPU_SEP );
#ifdef _WIN32
   /* MSYS2 and Cygwin shells hand out forward slashes, and GetModuleFileNameA
    * can return either depending on how the process was started. */
   {
      char *fwd = strrchr( path, '/' );
      if ( fwd && ( !slash || fwd > slash ) )
         slash = fwd;
   }
#endif
   if ( !slash )
      return false;
   *slash = '\0';
   if ( strlen( path ) + 1 > len )
      return false;
   strcpy( out, path );
   return true;
}

/* Why a load failed. Only worth printing when the file was actually there,
 * which is what file_exists below decides: a library that is simply absent
 * is the ordinary case on a single-vendor machine and means nothing.
 *
 * Windows reports a number rather than a sentence, and the one worth
 * recognising is 126: the library is present and something it imports is
 * not. That is the documented AMD failure, where a DLL built against the 7.x
 * HIP runtime cannot find amdhip64_7.dll because a consumer driver installs
 * only the 6.x one. See doc/building.md. */
/* Why a load failed, and whether that reason is simply that this machine has
 * no runtime for that vendor. Both come out of one call because on Windows
 * the code and the message are one GetLastError, and anything in between can
 * replace it. */
struct load_failure
{
   char why[256];
   bool runtime_absent;
};

static void load_failure( struct load_failure *f )
{
#ifdef _WIN32
   DWORD e = GetLastError();

   snprintf( f->why, sizeof(f->why), "Windows error %lu", (unsigned long) e );
   /* ERROR_MOD_NOT_FOUND: the library is there and something it imports is
    * not. For these two that import is the vendor's own runtime, which
    * arrives with the vendor's driver. */
   f->runtime_absent = ( e == ERROR_MOD_NOT_FOUND );
#else
   const char *e = dlerror();

   snprintf( f->why, sizeof(f->why), "%s", e ? e : "no reason given" );
   f->runtime_absent = e && strstr( e, "cannot open shared object file" ) != NULL;
#endif
}

static bool file_exists( const char *path )
{
#ifdef _WIN32
   return GetFileAttributesA( path ) != INVALID_FILE_ATTRIBUTES;
#else
   return access( path, F_OK ) == 0;
#endif
}

/* Every entry point, or none: a library missing one of these is not a
 * backend, and taking it half-loaded would crash on the first batch rather
 * than at startup where it can be explained. */
static bool bind_entry_points( struct backend *b )
{
   b->scan_batch      = (avian_gpu_scan_fn)   AVIAN_GPU_DLSYM( b->handle, "avian_cuda_scan_batch" );
   b->device_count    = (avian_gpu_count_fn)  AVIAN_GPU_DLSYM( b->handle, "avian_cuda_device_count" );
   b->device_name     = (avian_gpu_name_fn)   AVIAN_GPU_DLSYM( b->handle, "avian_cuda_device_name" );
   b->set_logger      = (avian_gpu_logger_fn) AVIAN_GPU_DLSYM( b->handle, "avian_cuda_set_logger" );
   b->set_diagnostics = (avian_gpu_diag_fn)   AVIAN_GPU_DLSYM( b->handle, "avian_cuda_set_diagnostics" );

   return b->scan_batch && b->device_count && b->device_name
       && b->set_logger && b->set_diagnostics;
}

/* A backend built against a different revision of cuda/avian-cuda-api.h is
 * refused rather than called. The two are separate files that can be updated
 * independently, and the struct they pass is plain data with no self
 * description, so a stale one would be read with the fields in the wrong
 * places: the device index arriving as a thread id, and no sign of it but
 * wrong answers. */
static bool abi_matches( struct backend *b )
{
   avian_gpu_abi_fn version
      = (avian_gpu_abi_fn) AVIAN_GPU_DLSYM( b->handle, "avian_cuda_abi_version" );
   int got;

   if ( !version )
   {
      applog( LOG_WARNING, "%s backend %s predates the versioned ABI; ignoring it",
              b->vendor, b->library );
      return false;
   }
   got = version();
   if ( got != AVIAN_CUDA_ABI_VERSION )
   {
      applog( LOG_WARNING, "%s backend %s speaks ABI %d, this miner speaks %d; "
                           "ignoring it. Rebuild the two together",
              b->vendor, b->library, got, AVIAN_CUDA_ABI_VERSION );
      return false;
   }
   return true;
}

static void load_backend( struct backend *b, avian_cuda_log_fn log )
{
   char dir[1024];
   char path[1280];
   struct load_failure f;
   bool beside_us = false;

   f.why[0] = 0;
   f.runtime_absent = false;
   if ( exe_directory( dir, sizeof(dir) ) )
   {
      snprintf( path, sizeof(path), "%s%c%s", dir, AVIAN_GPU_SEP, b->library );
      beside_us = file_exists( path );
      b->handle = AVIAN_GPU_DLOPEN( path );
      /* Kept now, because the fallback below overwrites it and the reason a
       * library beside the miner would not load is the one worth having. */
      if ( !b->handle && beside_us )
         load_failure( &f );
   }
   if ( !b->handle )
      b->handle = AVIAN_GPU_DLOPEN( b->library );
   b->found = beside_us || b->handle != NULL;
   if ( !b->handle )
   {
      /* Three cases, and only one of them is a fault.
       *
       * Absent is ordinary and deserves no line. So is present but wanting a
       * runtime this machine has not got: a package ships both backends, so
       * on any single-vendor machine the other vendor's library is always
       * there and never loads. Calling that an error would put a red line in
       * front of very nearly every user, which is how a diagnostic stops
       * being read at all.
       *
       * Anything else is a real fault, and saying nothing about it is what
       * made the first one of these take a debugger to find rather than
       * reading the miner's own output. */
      if ( beside_us && !f.runtime_absent )
         applog( LOG_ERR, "%s backend %s is there and will not load: %s",
                 b->vendor, path, f.why );
      else if ( beside_us )
         applog( LOG_DEBUG, "no %s runtime on this machine, so %s was not used",
                 b->vendor, b->library );
      return;
   }

   /* Two separate refusals, each saying so. Folded into one test, a library
    * missing any entry point but scan_batch was dropped without a word,
    * which is the same silence this file has already been caught in once. */
   if ( !bind_entry_points( b ) )
   {
      applog( LOG_WARNING, "%s backend %s is missing an entry point; ignoring it",
              b->vendor, b->library );
      AVIAN_GPU_DLCLOSE( b->handle );
      b->handle = NULL;
      return;
   }
   if ( !abi_matches( b ) )     /* which has said why */
   {
      AVIAN_GPU_DLCLOSE( b->handle );
      b->handle = NULL;
      return;
   }

   /* Before device_count, which is what takes the inventory and has plenty
    * to say if a driver is the wrong generation. Set afterwards, those
    * messages would be the ones lost. */
   b->set_logger( log );

   b->devices = b->device_count();
   if ( b->devices < 0 )
      b->devices = 0;
}

int avian_gpu_init( avian_cuda_log_fn log )
{
   if ( s_initialised )
      return s_devices;
   s_initialised = true;

   for ( size_t i = 0; i < sizeof(s_backend) / sizeof(s_backend[0]); i++ )
   {
      struct backend *b = &s_backend[i];

      load_backend( b, log );
      for ( int d = 0; d < b->devices; d++ )
      {
         if ( s_devices >= AVIAN_GPU_MAX_DEVICES )
         {
            applog( LOG_WARNING, "more than %d GPUs; the rest are ignored",
                    AVIAN_GPU_MAX_DEVICES );
            b->devices = d;
            break;
         }
         s_device[s_devices].backend = b;
         s_device[s_devices].local   = d;
         s_device[s_devices].shown   = s_devices;
         s_devices++;
      }
   }
   return s_devices;
}

int avian_gpu_device_count( void )
{
   return s_devices;
}

/* Narrow the list to the devices named in spec, a comma separated list of
 * numbers as avian_gpu_init numbered them, optionally with ranges: "0", "1,3",
 * "0-2,5". Returns how many are left, or -1 with a reason logged.
 *
 * Filtering here rather than in the gate keeps the mapping in the one place
 * that knows it. Everything above this file indexes devices 0..n-1 and never
 * learns that a number moved, while a log line can still say "GPU #3" of the
 * card the operator named, through avian_gpu_device_index.
 *
 * The case this exists for is an integrated GPU that enumerates alongside the
 * cards you meant to use. A package carrying both vendors' backends makes that
 * more likely, not less. */
int avian_gpu_select( const char *spec )
{
   bool keep[AVIAN_GPU_MAX_DEVICES] = { false };
   const char *p = spec;
   int kept = 0;

   if ( !spec || !*spec )
   {
      applog( LOG_ERR, "--devices was given nothing to select" );
      return -1;
   }
   while ( *p )
   {
      char *end;
      long lo = strtol( p, &end, 10 );
      long hi = lo;

      if ( end == p )
      {
         applog( LOG_ERR, "--devices wants numbers separated by commas, "
                          "not '%s'", spec );
         return -1;
      }
      if ( *end == '-' )
      {
         p = end + 1;
         hi = strtol( p, &end, 10 );
         if ( end == p || hi < lo )
         {
            applog( LOG_ERR, "--devices range '%s' does not run forwards",
                    spec );
            return -1;
         }
      }
      if ( lo < 0 || hi >= s_devices )
      {
         /* Named rather than clamped: asking for a card that is not there
          * means the command line and the machine disagree, and mining on
          * whatever happens to be present instead is how a rig quietly runs
          * at half rate. */
         applog( LOG_ERR, "--devices asked for %ld but this machine has %d "
                          "GPU%s, numbered 0 to %d", hi, s_devices,
                 s_devices == 1 ? "" : "s", s_devices - 1 );
         return -1;
      }
      for ( long i = lo; i <= hi; i++ )
         keep[i] = true;
      p = end;
      if ( *p == ',' )
         p++;
      else if ( *p )
      {
         applog( LOG_ERR, "--devices wants numbers separated by commas, "
                          "not '%s'", spec );
         return -1;
      }
   }
   for ( int i = 0; i < s_devices; i++ )
      if ( keep[i] )
         s_device[kept++] = s_device[i];
   if ( kept == 0 )
   {
      applog( LOG_ERR, "--devices selected none of the %d GPUs", s_devices );
      return -1;
   }
   s_devices = kept;
   return s_devices;
}

/* The device's number before any narrowing, for logs only. */
int avian_gpu_device_index( int device )
{
   if ( device < 0 || device >= s_devices )
      return -1;
   return s_device[device].shown;
}

int avian_gpu_backend_count( void )
{
   int n = 0;

   for ( size_t i = 0; i < sizeof(s_backend) / sizeof(s_backend[0]); i++ )
      if ( s_backend[i].handle )
         n++;
   return n;
}

int avian_gpu_backend_present( void )
{
   int n = 0;

   for ( size_t i = 0; i < sizeof(s_backend) / sizeof(s_backend[0]); i++ )
      if ( s_backend[i].found )
         n++;
   return n;
}

const char *avian_gpu_device_name( int device )
{
   if ( device < 0 || device >= s_devices )
      return "";
   return s_device[device].backend->device_name( s_device[device].local );
}

const char *avian_gpu_vendor( int device )
{
   if ( device < 0 || device >= s_devices )
      return "";
   return s_device[device].backend->vendor;
}

void avian_gpu_set_diagnostics( avian_cuda_hash_n_fn hash_n,
                                avian_cuda_prehash_fn prehash )
{
   for ( size_t i = 0; i < sizeof(s_backend) / sizeof(s_backend[0]); i++ )
      if ( s_backend[i].handle )
         s_backend[i].set_diagnostics( hash_n, prehash );
}

int avian_gpu_scan_batch( int device, avian_cuda_job *job,
                          uint32_t *out_nonces, int max_out,
                          uint32_t *throughput_used )
{
   if ( device < 0 || device >= s_devices )
      return -1;

   /* The backend's own index, not the miner's. Set here because this is the
    * only layer that knows the mapping, and passed in the job rather than
    * kept in a table on the far side: the backends each have their own
    * device_map, so a thread id means nothing to the one it does not
    * belong to. */
   job->device = s_device[device].local;

   return s_device[device].backend->scan_batch( job, out_nonces, max_out,
                                                throughput_used );
}

/*
 * Temperature, power and clock readings for the API.
 *
 * Copyright 2026 The avian-miner developers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * Where the numbers come from:
 *
 *   Nvidia cards     NVML, the library every GPU miner reads, loaded at run
 *                    time so the build needs no SDK and a machine without
 *                    the driver loses the readings and nothing else.
 *   AMD cards        Linux only: the hwmon node the amdgpu driver puts under
 *                    the card's PCI device. There is no ADL here.
 *   CPU on Linux     The k10temp, zenpower or coretemp driver, found by name
 *                    rather than by hwmon index, which changes between
 *                    boots. Package power from RAPL when it is readable.
 *   CPU on Windows   The effective clock from the performance counters,
 *                    which is real. Temperature and power only from
 *                    LibreHardwareMonitor's web server while that program
 *                    is running with it switched on: Windows gives user
 *                    mode nothing for either,
 *                    and the tools that show them each ship a kernel driver
 *                    to read the MSRs and SMN. A miner carrying a ring-0
 *                    driver is a different kind of program, and the one that
 *                    antivirus is right to flag, so this does not.
 *
 * Readings are cached for a second. The API answers whenever a dashboard
 * polls, and two of them polling should not double the driver traffic.
 *
 * Nothing from miner.h is used, so this compiles and runs on its own, which
 * is how it was tested.
 */

#include "sensors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <pdh.h>
#  include <pdhmsg.h>
#  include <curl/curl.h>
#  include <jansson.h>
#else
#  include <dlfcn.h>
#  include <dirent.h>
#  include <unistd.h>
#  include <time.h>
#endif

#define MAX_GPUS      32
#define CACHE_MS    1000
#define MAX_LOGICAL  256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void ( *log_fn )( int level, const char *line );

void sensors_set_log( void ( *fn )( int level, const char *line ) )
{
   log_fn = fn;
}

static void note( int level, const char *fmt, ... )
{
   char line[ 256 ];
   va_list ap;

   if ( !log_fn ) return;
   va_start( ap, fmt );
   vsnprintf( line, sizeof line, fmt, ap );
   va_end( ap );
   log_fn( level, line );
}

static uint64_t now_ms( void )
{
#if defined(_WIN32)
   return GetTickCount64();
#else
   struct timespec ts;

   clock_gettime( CLOCK_MONOTONIC, &ts );
   return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void gpu_clear( struct gpu_sensors *g )
{
   g->temp = g->mem_temp = g->hotspot = g->power = -1.0f;
   g->fan = g->clock = g->mem_clock = -1;
}

static bool gpu_any( const struct gpu_sensors *g )
{
   return g->temp >= 0 || g->mem_temp >= 0 || g->hotspot >= 0 || g->fan >= 0
       || g->power >= 0 || g->clock >= 0 || g->mem_clock >= 0;
}

static void cpu_clear( struct cpu_sensors *c )
{
   memset( c, 0, sizeof *c );
   c->source = NULL;
   c->temp = c->power = -1.0f;
}

/* "Tccd1" from the kernel and "CCD1 (Tdie)" from LibreHardwareMonitor are
 * the same die; the page should not have to know two spellings. */
static void zone_add( struct cpu_sensors *c, const char *label, float temp )
{
   char  name[64];
   size_t i;

   if ( c->nzones >= CPU_SENSOR_ZONES || temp <= 0 ) return;

   if ( !strncmp( label, "Tccd", 4 ) )
      snprintf( name, sizeof name, "CCD%s", label + 4 );
   else
      snprintf( name, sizeof name, "%s", label );
   for ( i = 0; name[i]; i++ )
      if ( name[i] == ' ' || name[i] == ';' || name[i] == '|' || name[i] == '=' )
      {
         name[i] = '\0';
         break;
      }

   snprintf( c->zone[ c->nzones ].label, sizeof c->zone[0].label, "%.15s", name );
   c->zone[ c->nzones ].temp = temp;
   c->nzones++;
}

/* ------------------------------------------------------------------ */
/* NVML, by dlopen                                                      */
/* ------------------------------------------------------------------ */

typedef void *nvml_dev;
typedef int   nvml_ret;                 /* 0 is NVML_SUCCESS */

enum { NVML_CLOCK_SM = 1, NVML_CLOCK_MEM = 2 };
enum { NVML_FI_DEV_MEMORY_TEMP = 82 };
enum
{
   NVML_VALUE_TYPE_DOUBLE = 0,
   NVML_VALUE_TYPE_UNSIGNED_INT = 1,
   NVML_VALUE_TYPE_UNSIGNED_LONG = 2,
   NVML_VALUE_TYPE_UNSIGNED_LONG_LONG = 3,
   NVML_VALUE_TYPE_SIGNED_LONG_LONG = 4,
   NVML_VALUE_TYPE_SIGNED_INT = 5
};

/* nvmlFieldValue_t, 40 bytes on both platforms. The only one of NVML's
 * structs used here, and the one that gets the memory temperature, which
 * has no function of its own. */
typedef struct
{
   unsigned  fieldId;
   unsigned  scopeId;
   long long timestamp;
   long long latencyUsec;
   int       valueType;
   nvml_ret  nvmlReturn;
   union
   {
      double             dVal;
      unsigned           uiVal;
      unsigned long      ulVal;
      unsigned long long ullVal;
      long long          sllVal;
      int                siVal;
   } value;
} nvml_field;

static struct
{
   void *lib;
   bool  tried, ok;
   nvml_ret (*init)( void );
   nvml_ret (*by_bus)( const char *, nvml_dev * );
   nvml_ret (*temp)( nvml_dev, int, unsigned * );
   nvml_ret (*fan)( nvml_dev, unsigned * );
   nvml_ret (*power)( nvml_dev, unsigned * );
   nvml_ret (*clock)( nvml_dev, int, unsigned * );
   nvml_ret (*fields)( nvml_dev, int, nvml_field * );
} nvml;

static struct { int bus; nvml_dev dev; } nv_dev[ MAX_GPUS ];
static int n_nv_dev;

static void *lib_open( const char *name )
{
#if defined(_WIN32)
   return (void *) LoadLibraryA( name );
#else
   return dlopen( name, RTLD_NOW );
#endif
}

static void *lib_sym( void *lib, const char *name )
{
#if defined(_WIN32)
   return (void *) (uintptr_t) GetProcAddress( (HMODULE) lib, name );
#else
   return dlsym( lib, name );
#endif
}

#define SYM( field, name ) ( *(void **) &nvml.field = lib_sym( nvml.lib, name ) )

static bool nvml_load( void )
{
   if ( nvml.tried ) return nvml.ok;
   nvml.tried = true;

#if defined(_WIN32)
   /* The driver puts nvml.dll in System32 now; older ones only in NVSMI. */
   nvml.lib = lib_open( "nvml.dll" );
   if ( !nvml.lib )
   {
      const char *pf = getenv( "ProgramFiles" );
      char path[ MAX_PATH ];

      if ( pf )
      {
         snprintf( path, sizeof path,
                   "%s\\NVIDIA Corporation\\NVSMI\\nvml.dll", pf );
         nvml.lib = lib_open( path );
      }
   }
#else
   nvml.lib = lib_open( "libnvidia-ml.so.1" );
   if ( !nvml.lib ) nvml.lib = lib_open( "libnvidia-ml.so" );
#endif
   if ( !nvml.lib )
   {
      note( SENSORS_LOG_DEBUG, "sensors: NVML not found, so no Nvidia card readings" );
      return false;
   }

   if ( !SYM( init,   "nvmlInit_v2" ) )                      SYM( init,   "nvmlInit" );
   if ( !SYM( by_bus, "nvmlDeviceGetHandleByPciBusId_v2" ) ) SYM( by_bus, "nvmlDeviceGetHandleByPciBusId" );
   SYM( temp,   "nvmlDeviceGetTemperature" );
   SYM( fan,    "nvmlDeviceGetFanSpeed" );
   SYM( power,  "nvmlDeviceGetPowerUsage" );
   SYM( clock,  "nvmlDeviceGetClockInfo" );
   SYM( fields, "nvmlDeviceGetFieldValues" );

   if ( !nvml.init || !nvml.by_bus ) return false;
   nvml.ok = nvml.init() == 0;
   if ( nvml.ok ) note( SENSORS_LOG_INFO, "sensors: Nvidia cards report through NVML" );
   else           note( SENSORS_LOG_INFO, "sensors: NVML would not initialise, so no Nvidia card readings" );
   return nvml.ok;
}

/* The device on a bus, looked up once. A bus NVML does not know -- an AMD
 * card, say -- is remembered as NULL so it is not asked again. */
static nvml_dev nvml_device( int bus )
{
   char id[ 32 ];
   nvml_dev dev = NULL;
   int i;

   for ( i = 0; i < n_nv_dev; i++ )
      if ( nv_dev[i].bus == bus ) return nv_dev[i].dev;
   if ( n_nv_dev >= MAX_GPUS ) return NULL;

   snprintf( id, sizeof id, "%04x:%02x:00.0", 0, bus );
   if ( nvml.by_bus( id, &dev ) != 0 )
   {
      dev = NULL;
      note( SENSORS_LOG_DEBUG, "sensors: NVML knows no card on bus %d", bus );
   }

   nv_dev[ n_nv_dev ].bus = bus;
   nv_dev[ n_nv_dev ].dev = dev;
   n_nv_dev++;
   return dev;
}

static void nvml_read( int bus, struct gpu_sensors *g )
{
   nvml_dev d = nvml_device( bus );
   unsigned v;

   if ( !d ) return;

   if ( nvml.temp  && nvml.temp( d, 0, &v ) == 0 )  g->temp  = (float) v;
   if ( nvml.fan   && nvml.fan( d, &v ) == 0 )      g->fan   = (int) v;
   if ( nvml.power && nvml.power( d, &v ) == 0 )    g->power = v / 1000.0f;
   if ( nvml.clock )
   {
      if ( nvml.clock( d, NVML_CLOCK_SM,  &v ) == 0 ) g->clock     = (int) v;
      if ( nvml.clock( d, NVML_CLOCK_MEM, &v ) == 0 ) g->mem_clock = (int) v;
   }
   if ( nvml.fields )
   {
      nvml_field f;
      double t = 0;

      memset( &f, 0, sizeof f );
      f.fieldId = NVML_FI_DEV_MEMORY_TEMP;
      if ( nvml.fields( d, 1, &f ) == 0 && f.nvmlReturn == 0 )
      {
         switch ( f.valueType )
         {
            case NVML_VALUE_TYPE_DOUBLE:             t = f.value.dVal;   break;
            case NVML_VALUE_TYPE_UNSIGNED_INT:       t = f.value.uiVal;  break;
            case NVML_VALUE_TYPE_UNSIGNED_LONG:      t = f.value.ulVal;  break;
            case NVML_VALUE_TYPE_UNSIGNED_LONG_LONG: t = (double) f.value.ullVal; break;
            case NVML_VALUE_TYPE_SIGNED_LONG_LONG:   t = (double) f.value.sllVal; break;
            case NVML_VALUE_TYPE_SIGNED_INT:         t = f.value.siVal;  break;
         }
         /* Boards without the sensor answer 0 rather than an error. */
         if ( t > 0 && t < 200 ) g->mem_temp = (float) t;
      }
   }
}

/* ------------------------------------------------------------------ */
/* sysfs, Linux                                                         */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)

static bool read_line( const char *path, char *out, size_t n )
{
   FILE *f = fopen( path, "r" );
   size_t len;

   if ( !f ) return false;
   if ( !fgets( out, (int) n, f ) ) { fclose( f ); return false; }
   fclose( f );
   len = strlen( out );
   while ( len && ( out[ len - 1 ] == '\n' || out[ len - 1 ] == ' ' ) )
      out[ --len ] = '\0';
   return len > 0;
}

static bool read_long( const char *path, long long *out )
{
   char buf[ 64 ], *end;

   if ( !read_line( path, buf, sizeof buf ) ) return false;
   *out = strtoll( buf, &end, 10 );
   return end != buf;
}

/* /sys/bus/pci/devices/0000:0b:00.0/hwmon/hwmon3 for bus 0x0b, found by
 * walking the device list, since the device and function are not known
 * here and the hwmon number is whatever it was at boot. */
static struct { int bus; char dev[ 300 ]; char hw[ 340 ]; } pci_hw[ MAX_GPUS ];
static int n_pci_hw;

static bool pci_hwmon( int bus, const char **dev, const char **hw )
{
   DIR *d;
   struct dirent *e;
   int i;

   for ( i = 0; i < n_pci_hw; i++ )
      if ( pci_hw[i].bus == bus )
      {
         *dev = pci_hw[i].dev;
         *hw  = pci_hw[i].hw;
         return pci_hw[i].hw[0] != '\0';
      }
   if ( n_pci_hw >= MAX_GPUS ) return false;

   pci_hw[ n_pci_hw ].bus = bus;
   pci_hw[ n_pci_hw ].dev[0] = pci_hw[ n_pci_hw ].hw[0] = '\0';

   d = opendir( "/sys/bus/pci/devices" );
   if ( d )
   {
      while ( ( e = readdir( d ) ) )
      {
         unsigned dom, b, dv, fn;
         char path[ 300 ];
         DIR *hd;
         struct dirent *he;

         if ( sscanf( e->d_name, "%x:%x:%x.%x", &dom, &b, &dv, &fn ) != 4 )
            continue;
         if ( (int) b != bus ) continue;

         snprintf( path, sizeof path, "/sys/bus/pci/devices/%s/hwmon",
                   e->d_name );
         hd = opendir( path );
         if ( !hd ) continue;
         while ( ( he = readdir( hd ) ) )
            if ( !strncmp( he->d_name, "hwmon", 5 ) )
            {
               snprintf( pci_hw[ n_pci_hw ].dev, sizeof pci_hw[0].dev,
                         "/sys/bus/pci/devices/%s", e->d_name );
               snprintf( pci_hw[ n_pci_hw ].hw, sizeof pci_hw[0].hw, "%s/%s",
                         path, he->d_name );
               break;
            }
         closedir( hd );
         if ( pci_hw[ n_pci_hw ].hw[0] ) break;
      }
      closedir( d );
   }

   *dev = pci_hw[ n_pci_hw ].dev;
   *hw  = pci_hw[ n_pci_hw ].hw;
   n_pci_hw++;
   if ( ( *hw )[0] )
      note( SENSORS_LOG_INFO, "sensors: the card on bus %d reports through %s", bus, *hw );
   else
      note( SENSORS_LOG_DEBUG, "sensors: no hwmon under the PCI device on bus %d", bus );
   return ( *hw )[0] != '\0';
}

/* The level marked * in a pp_dpm_sclk listing: "1: 2044Mhz *". */
static int amd_dpm_clock( const char *dev, const char *file )
{
   char path[ 340 ], line[ 64 ];
   FILE *f;
   int mhz = -1;

   snprintf( path, sizeof path, "%s/%s", dev, file );
   f = fopen( path, "r" );
   if ( !f ) return -1;
   while ( fgets( line, sizeof line, f ) )
   {
      int level, v;

      if ( strchr( line, '*' ) && sscanf( line, "%d: %dMhz", &level, &v ) == 2 )
      {
         mhz = v;
         break;
      }
   }
   fclose( f );
   return mhz;
}

static void amd_read( int bus, struct gpu_sensors *g )
{
   const char *dev, *hw;
   char path[ 380 ], label[ 32 ];
   long long v;
   int i;

   if ( !pci_hwmon( bus, &dev, &hw ) ) return;

   for ( i = 1; i <= 8; i++ )
   {
      float t;

      snprintf( path, sizeof path, "%s/temp%d_input", hw, i );
      if ( !read_long( path, &v ) ) continue;
      t = v / 1000.0f;

      snprintf( path, sizeof path, "%s/temp%d_label", hw, i );
      if ( !read_line( path, label, sizeof label ) )
      {
         if ( i == 1 ) g->temp = t;
         continue;
      }
      if      ( !strcmp( label, "edge" ) )     g->temp     = t;
      else if ( !strcmp( label, "junction" ) ) g->hotspot  = t;
      else if ( !strcmp( label, "mem" ) )      g->mem_temp = t;
   }

   snprintf( path, sizeof path, "%s/power1_average", hw );
   if ( !read_long( path, &v ) )
   {
      snprintf( path, sizeof path, "%s/power1_input", hw );
      if ( !read_long( path, &v ) ) v = -1;
   }
   if ( v >= 0 ) g->power = v / 1000000.0f;

   snprintf( path, sizeof path, "%s/pwm1", hw );
   if ( read_long( path, &v ) && v >= 0 ) g->fan = (int) ( v * 100 / 255 );

   g->clock     = amd_dpm_clock( dev, "pp_dpm_sclk" );
   g->mem_clock = amd_dpm_clock( dev, "pp_dpm_mclk" );
}

/* The CPU's hwmon device, by driver name. zenpower first because anyone
 * who loaded it did so to get what k10temp lacks, then k10temp, then Intel's
 * coretemp. */
static struct { bool tried; char dir[ 64 ]; char name[ 32 ]; } cpu_hw;

static void linux_cpu_find( void )
{
   static const char *const want[] = { "zenpower", "k10temp", "coretemp" };
   DIR *d;
   struct dirent *e;
   int best = 99;

   if ( cpu_hw.tried ) return;
   cpu_hw.tried = true;

   d = opendir( "/sys/class/hwmon" );
   if ( !d ) return;
   while ( ( e = readdir( d ) ) )
   {
      char path[ 96 ], name[ 32 ];
      int i;

      if ( strncmp( e->d_name, "hwmon", 5 ) ) continue;
      snprintf( path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name );
      if ( !read_line( path, name, sizeof name ) ) continue;
      for ( i = 0; i < 3; i++ )
         if ( !strcmp( name, want[i] ) && i < best )
         {
            best = i;
            snprintf( cpu_hw.dir, sizeof cpu_hw.dir, "/sys/class/hwmon/%s",
                      e->d_name );
            snprintf( cpu_hw.name, sizeof cpu_hw.name, "%s", name );
         }
   }
   closedir( d );

   if ( cpu_hw.dir[0] )
      note( SENSORS_LOG_INFO, "sensors: the CPU reports through %s at %s",
            cpu_hw.name, cpu_hw.dir );
   else
      note( SENSORS_LOG_INFO, "sensors: no CPU temperature driver found "
            "(k10temp, zenpower or coretemp), so none is reported" );
}

/* Package power from RAPL as an energy delta, which needs two reads a
 * second or more apart; the first call after a long gap sets the baseline
 * and reports nothing. Zen 2 and later show up under the intel-rapl name
 * too. Root-only on most kernels since 5.10, so often absent on a desktop
 * and present on a rig. */
static struct
{
   bool      have;
   long long last_uj;
   uint64_t  last_ms;
   float     watts;
} rapl = { false, 0, 0, -1.0f };

static float rapl_power( void )
{
   static const char *energy = "/sys/class/powercap/intel-rapl:0/energy_uj";
   static const char *range  = "/sys/class/powercap/intel-rapl:0/max_energy_range_uj";
   static bool said = false;
   long long uj, max;
   uint64_t t = now_ms();

   if ( !read_long( energy, &uj ) )
   {
      if ( !said )
         note( SENSORS_LOG_DEBUG, "sensors: %s is not readable, so no CPU power", energy );
      said = true;
      return -1.0f;
   }

   if ( rapl.have && t > rapl.last_ms )
   {
      long long d = uj - rapl.last_uj;

      if ( d < 0 && read_long( range, &max ) ) d += max;
      /* Microjoules over milliseconds is milliwatts. */
      if ( d >= 0 && t - rapl.last_ms >= 900 )
      {
         rapl.watts   = (float) ( d / (double) ( t - rapl.last_ms ) / 1000.0 );
         rapl.last_uj = uj;
         rapl.last_ms = t;
      }
   }
   else
   {
      rapl.have    = true;
      rapl.last_uj = uj;
      rapl.last_ms = t;
   }
   return rapl.watts;
}

static void linux_cpu_read( struct cpu_sensors *c )
{
   char path[ 128 ], label[ 32 ];
   long long v;
   bool tdie = false;
   int i;

   linux_cpu_find();
   if ( cpu_hw.dir[0] )
   {
      c->source = cpu_hw.name;
      for ( i = 1; i <= 24; i++ )
      {
         float t;

         snprintf( path, sizeof path, "%s/temp%d_input", cpu_hw.dir, i );
         if ( !read_long( path, &v ) ) continue;
         t = v / 1000.0f;

         snprintf( path, sizeof path, "%s/temp%d_label", cpu_hw.dir, i );
         if ( !read_line( path, label, sizeof label ) )
         {
            if ( c->temp < 0 ) c->temp = t;
            continue;
         }
         /* Tdie is the real die temperature; Tctl adds an offset on some
          * parts for the fan curve. Take Tdie when both are there. */
         if ( !strcmp( label, "Tdie" ) )             { c->temp = t; tdie = true; }
         else if ( !strcmp( label, "Tctl" ) )        { if ( !tdie ) c->temp = t; }
         else if ( !strncmp( label, "Tccd", 4 ) )    zone_add( c, label, t );
         else if ( !strncmp( label, "Package", 7 ) ) c->temp = t;
      }

      /* zenpower: core and SoC power, summed. */
      for ( i = 1; i <= 4; i++ )
      {
         snprintf( path, sizeof path, "%s/power%d_input", cpu_hw.dir, i );
         if ( read_long( path, &v ) && v >= 0 )
            c->power = ( c->power < 0 ? 0 : c->power ) + v / 1000000.0f;
      }
   }

   if ( c->power < 0 ) c->power = rapl_power();
}

static int linux_cpu_clock( int logical )
{
   char path[ 96 ];
   long long khz;

   snprintf( path, sizeof path,
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", logical );
   if ( !read_long( path, &khz ) || khz <= 0 ) return -1;
   return (int) ( khz / 1000 );
}

#endif /* !_WIN32 */

/* ------------------------------------------------------------------ */
/* Windows                                                              */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

/* \Processor Information(*)\% Processor Performance is the kernel's
 * APERF/MPERF ratio: how fast each logical CPU actually ran over the
 * interval, as a percentage of nominal, boost included. Times the nominal
 * frequency from the same counter set gives MHz. A rate counter needs two
 * collections, so the first call primes and the next ones answer. */
static struct
{
   bool         tried, ok;
   PDH_HQUERY   query;
   PDH_HCOUNTER perf, freq;
   uint64_t     at;
   int          mhz[ MAX_LOGICAL ];
} pdh;

static void pdh_open( void )
{
   int i;

   if ( pdh.tried ) return;
   pdh.tried = true;
   for ( i = 0; i < MAX_LOGICAL; i++ ) pdh.mhz[i] = -1;

   if ( PdhOpenQueryW( NULL, 0, &pdh.query ) == ERROR_SUCCESS
        && PdhAddEnglishCounterW( pdh.query,
              L"\\Processor Information(*)\\% Processor Performance", 0,
              &pdh.perf ) == ERROR_SUCCESS
        && PdhAddEnglishCounterW( pdh.query,
              L"\\Processor Information(*)\\Processor Frequency", 0,
              &pdh.freq ) == ERROR_SUCCESS
        && PdhCollectQueryData( pdh.query ) == ERROR_SUCCESS )
   {
      pdh.at = now_ms();
      pdh.ok = true;
      note( SENSORS_LOG_DEBUG, "sensors: CPU clocks from the Processor Information counters" );
   }
   else
      note( SENSORS_LOG_INFO, "sensors: the Processor Information counters are not available, so no CPU clocks" );
}

/* Instance names are "group,processor"; "_Total" and "group,_Total" are
 * skipped. A group is 64 logical CPUs. */
static int pdh_instance( const wchar_t *name )
{
   unsigned g = 0, n = 0;

   if ( wcschr( name, L'T' ) ) return -1;
   if ( swscanf( name, L"%u,%u", &g, &n ) != 2 ) return -1;
   if ( g * 64 + n >= MAX_LOGICAL ) return -1;
   return (int) ( g * 64 + n );
}

static PDH_FMT_COUNTERVALUE_ITEM_W *pdh_array( PDH_HCOUNTER h, DWORD *count )
{
   DWORD size = 0;
   PDH_FMT_COUNTERVALUE_ITEM_W *items;

   *count = 0;
   if ( PdhGetFormattedCounterArrayW( h, PDH_FMT_DOUBLE, &size, count, NULL )
        != (PDH_STATUS) PDH_MORE_DATA || !size )
      return NULL;
   items = malloc( size );
   if ( !items ) return NULL;
   if ( PdhGetFormattedCounterArrayW( h, PDH_FMT_DOUBLE, &size, count, items )
        != ERROR_SUCCESS )
   {
      free( items );
      *count = 0;
      return NULL;
   }
   return items;
}

static void pdh_sample( void )
{
   PDH_FMT_COUNTERVALUE_ITEM_W *perf, *freq;
   DWORD nperf, nfreq, i;
   double nominal[ MAX_LOGICAL ];

   pdh_open();
   if ( !pdh.ok ) return;
   if ( now_ms() - pdh.at < 900 ) return;
   if ( PdhCollectQueryData( pdh.query ) != ERROR_SUCCESS ) return;
   pdh.at = now_ms();

   for ( i = 0; i < MAX_LOGICAL; i++ ) nominal[i] = 0;
   freq = pdh_array( pdh.freq, &nfreq );
   for ( i = 0; i < nfreq; i++ )
   {
      int idx = pdh_instance( freq[i].szName );

      if ( idx >= 0 && freq[i].FmtValue.CStatus == ERROR_SUCCESS )
         nominal[ idx ] = freq[i].FmtValue.doubleValue;
   }
   free( freq );

   perf = pdh_array( pdh.perf, &nperf );
   for ( i = 0; i < nperf; i++ )
   {
      int idx = pdh_instance( perf[i].szName );

      if ( idx >= 0 && perf[i].FmtValue.CStatus == ERROR_SUCCESS
           && nominal[ idx ] > 0 )
         pdh.mhz[ idx ] = (int) ( perf[i].FmtValue.doubleValue
                                  * nominal[ idx ] / 100.0 + 0.5 );
   }
   free( perf );
}

/* LibreHardwareMonitor serves everything it reads as one JSON tree at
 * http://127.0.0.1:8085/data.json once its web server is switched on, under
 * Options > Remote Web Server > Run. That is the only way out of it now: the
 * WMI namespace this used to read went with 0.9.5, and a copy downloaded
 * today has no provider in it at all. A refused connection is the program
 * not running or the server not switched on, and both are tried again every
 * thirty seconds so doing either later is enough. */
#define LHM_URL "http://127.0.0.1:8085/data.json"
#define LHM_MAX (4u << 20)

static struct
{
   CURL    *curl;
   char    *buf;
   size_t   len, cap;
   uint64_t retry_at;
   enum { LHM_UNKNOWN, LHM_UP, LHM_DOWN } state;
} lhm;

static size_t lhm_sink( char *p, size_t size, size_t n, void *unused )
{
   size_t add = size * n;

   (void) unused;
   if ( lhm.len + add + 1 > lhm.cap )
   {
      size_t cap = lhm.cap ? lhm.cap : 65536;
      char *grown;

      while ( cap < lhm.len + add + 1 ) cap *= 2;
      /* Megabytes from port 8085 is something other than the document
       * expected, and refusing it makes the transfer fail rather than
       * grow. */
      if ( cap > LHM_MAX ) return 0;
      grown = realloc( lhm.buf, cap );
      if ( !grown ) return 0;
      lhm.buf = grown;
      lhm.cap = cap;
   }
   memcpy( lhm.buf + lhm.len, p, add );
   lhm.len += add;
   lhm.buf[ lhm.len ] = '\0';
   return add;
}

static void lhm_down( const char *why )
{
   if ( lhm.state != LHM_DOWN )
      note( SENSORS_LOG_INFO, "sensors: LibreHardwareMonitor is not answering "
            "at " LHM_URL " (%s), so no CPU temperature or power. Start it, "
            "turn on Options > Remote Web Server > Run, and they appear", why );
   lhm.state = LHM_DOWN;
   lhm.retry_at = now_ms() + 30000;
}

/* The whole tree, or NULL. The miner calls curl_global_init before any
 * thread starts, as libcurl requires; a standalone build of this file has
 * to do the same. */
static json_t *lhm_fetch( void )
{
   json_error_t err;
   json_t *root;
   CURLcode res;

   if ( lhm.state == LHM_DOWN && now_ms() < lhm.retry_at ) return NULL;
   if ( !lhm.curl )
   {
      lhm.curl = curl_easy_init();
      if ( !lhm.curl ) return NULL;
      curl_easy_setopt( lhm.curl, CURLOPT_URL, LHM_URL );
      curl_easy_setopt( lhm.curl, CURLOPT_WRITEFUNCTION, lhm_sink );
      curl_easy_setopt( lhm.curl, CURLOPT_CONNECTTIMEOUT_MS, 500L );
      curl_easy_setopt( lhm.curl, CURLOPT_TIMEOUT_MS, 2000L );
      curl_easy_setopt( lhm.curl, CURLOPT_FAILONERROR, 1L );
      curl_easy_setopt( lhm.curl, CURLOPT_NOSIGNAL, 1L );
      curl_easy_setopt( lhm.curl, CURLOPT_NOPROXY, "*" );
   }
   lhm.len = 0;
   res = curl_easy_perform( lhm.curl );
   if ( res != CURLE_OK )
   {
      lhm_down( curl_easy_strerror( res ) );
      return NULL;
   }
   root = json_loadb( lhm.buf ? lhm.buf : "", lhm.len, 0, &err );
   if ( !root )
   {
      lhm_down( "not the JSON expected" );
      return NULL;
   }
   if ( lhm.state != LHM_UP )
      note( SENSORS_LOG_INFO,
            "sensors: the CPU reports through LibreHardwareMonitor" );
   lhm.state = LHM_UP;
   return root;
}

/* LibreHardwareMonitor's names for the CPU, AMD and Intel:
 *   temperature  "Core (Tctl/Tdie)"  "CCD1 (Tdie)"  "CPU Package"
 *   power        "Package"           "CPU Package"
 * The per-core ones and the CCD averages are left out. A sensor node
 * carries SensorId, Type and RawValue, the last a number where the Value
 * beside it is the formatted string the window shows. */
static void lhm_walk( json_t *node, struct cpu_sensors *c, bool *any )
{
   const char *id = json_string_value( json_object_get( node, "SensorId" ) );
   json_t *kids, *kid;
   size_t i;

   if ( id && ( !strncmp( id, "/amdcpu/", 8 )
                || !strncmp( id, "/intelcpu/", 10 ) ) )
   {
      const char *type = json_string_value( json_object_get( node, "Type" ) );
      const char *name = json_string_value( json_object_get( node, "Text" ) );
      json_t *raw = json_object_get( node, "RawValue" );
      double val = json_is_number( raw ) ? json_number_value( raw ) : 0;

      if ( type && name && val > 0 )
      {
         *any = true;
         if ( !strcmp( type, "Temperature" ) )
         {
            if ( !strncmp( name, "Core (Tctl", 10 )
                 || !strcmp( name, "CPU Package" ) )
               c->temp = (float) val;
            else if ( !strncmp( name, "CCD", 3 ) && !strstr( name, "Average" )
                      && !strstr( name, "Max" ) )
               zone_add( c, name, (float) val );
         }
         else if ( !strcmp( type, "Power" )
                   && ( !strcmp( name, "Package" )
                        || !strcmp( name, "CPU Package" ) ) )
            c->power = (float) val;
      }
   }
   kids = json_object_get( node, "Children" );
   json_array_foreach( kids, i, kid )
      if ( json_is_object( kid ) ) lhm_walk( kid, c, any );
}

static void lhm_read( struct cpu_sensors *c )
{
   json_t *root = lhm_fetch();
   bool any = false;

   if ( !root ) return;
   lhm_walk( root, c, &any );
   json_decref( root );
   if ( any ) c->source = "LibreHardwareMonitor";
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* the public three                                                     */
/* ------------------------------------------------------------------ */

static struct { int bus; uint64_t at; struct gpu_sensors g; } gpu_cache[ MAX_GPUS ];
static int n_gpu_cache;

bool sensors_gpu( int pci_bus, struct gpu_sensors *out )
{
   uint64_t t = now_ms();
   int i;

   gpu_clear( out );
   if ( pci_bus < 0 ) return false;

   pthread_mutex_lock( &lock );
   for ( i = 0; i < n_gpu_cache; i++ )
      if ( gpu_cache[i].bus == pci_bus ) break;
   if ( i == n_gpu_cache )
   {
      if ( n_gpu_cache >= MAX_GPUS )
      {
         pthread_mutex_unlock( &lock );
         return false;
      }
      gpu_cache[i].bus = pci_bus;
      gpu_cache[i].at  = 0;
      n_gpu_cache++;
   }

   if ( !gpu_cache[i].at || t - gpu_cache[i].at >= CACHE_MS )
   {
      gpu_clear( &gpu_cache[i].g );
      if ( nvml_load() ) nvml_read( pci_bus, &gpu_cache[i].g );
#if !defined(_WIN32)
      if ( !gpu_any( &gpu_cache[i].g ) ) amd_read( pci_bus, &gpu_cache[i].g );
#endif
      gpu_cache[i].at = t;
   }
   *out = gpu_cache[i].g;
   pthread_mutex_unlock( &lock );
   return gpu_any( out );
}

static struct { uint64_t at; struct cpu_sensors c; } cpu_cache;

void sensors_cpu( struct cpu_sensors *out )
{
   uint64_t t = now_ms();

   pthread_mutex_lock( &lock );
   if ( !cpu_cache.at || t - cpu_cache.at >= CACHE_MS )
   {
      cpu_clear( &cpu_cache.c );
#if defined(_WIN32)
      lhm_read( &cpu_cache.c );
#else
      linux_cpu_read( &cpu_cache.c );
#endif
      cpu_cache.at = t;
   }
   *out = cpu_cache.c;
   pthread_mutex_unlock( &lock );
}

int sensors_cpu_clock( int logical )
{
   int mhz;

   if ( logical < 0 || logical >= MAX_LOGICAL ) return -1;
   pthread_mutex_lock( &lock );
#if defined(_WIN32)
   pdh_sample();
   mhz = pdh.mhz[ logical ];
#else
   mhz = linux_cpu_clock( logical );
#endif
   pthread_mutex_unlock( &lock );
   return mhz;
}

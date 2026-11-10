/*
 * avian-sensors: CPU temperature and power for avian-miner on Windows.
 *
 * Copyright 2026 The avian-miner developers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * What it is: a Windows service that reads the CPU's package temperature,
 * per-die temperatures where the CPU has them, and package power, once a
 * second, and hands the latest reading to anything that opens its named
 * pipe. The miner is the thing that opens it; see avian-sensors.h for the
 * one struct that crosses.
 *
 * Why a service: the registers that hold these numbers are kernel-only.
 * PawnIO (pawnio.eu) is a signed driver that reads them on behalf of an
 * administrator, running small signed modules that each know one family of
 * hardware; LibreHardwareMonitor, HWiNFO's open-source cousin, is built on
 * it. Opening its device needs administrator rights, which a miner should
 * never have, so the reading happens here, as LocalSystem, and the miner
 * gets a pipe it can open as anybody.
 *
 * What it needs: PawnIO installed, and the two module files beside this
 * exe, AMDFamily17.bin and IntelMSR.bin, which are the signed builds
 * published at https://github.com/namazso/PawnIO.Modules/releases (LGPL).
 * The driver will not run a module that is not signed, so those files are
 * used as published and never edited. Without either, the service runs,
 * says what is missing, serves "unknown", and tries again every thirty
 * seconds.
 *
 * What it decodes, and from where: the register maps are LibreHardwareMonitor's
 * (Amd17Cpu.cs and IntelCpu.cs, MPL-2.0), which in turn follow the Linux
 * k10temp and coretemp drivers. A CPU family this does not know is left
 * unknown rather than guessed at: a confident wrong temperature is worse
 * than none.
 *
 *   avian-sensors --install     register and start the service (administrator)
 *   avian-sensors --uninstall   stop and remove it (administrator)
 *   avian-sensors --console     run in this window instead, for a look
 *   avian-sensors --fake        with either: invent readings, to test the pipe
 */

#include "avian-sensors.h"

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <sddl.h>
#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#  define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

#define SERVICE_NAME    "avian-sensors"
#define SERVICE_DISPLAY "Avian miner CPU sensors"
#define RETRY_MS        30000

static bool console_mode, fake_mode;
static HANDLE stop_event;
static CRITICAL_SECTION lock;
static struct avsens_snapshot snap;
static FILE *log_file;
static char exe_dir[ MAX_PATH + 1 ];

/* ------------------------------------------------------------------ */
/* saying things                                                        */
/* ------------------------------------------------------------------ */

static void say( const char *fmt, ... )
{
   SYSTEMTIME t;
   va_list ap;
   FILE *to = console_mode ? stderr : log_file;

   if ( !to ) return;
   GetLocalTime( &t );
   fprintf( to, "[%04d-%02d-%02d %02d:%02d:%02d] ", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond );
   va_start( ap, fmt );
   vfprintf( to, fmt, ap );
   va_end( ap );
   fputc( '\n', to );
   fflush( to );
}

static const char *win_err( DWORD e )
{
   static char buf[ 256 ];
   DWORD n = FormatMessageA( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, e, 0, buf, sizeof buf, NULL );

   while ( n && ( buf[ n - 1 ] == '\n' || buf[ n - 1 ] == '\r' || buf[ n - 1 ] == '.' ) )
      buf[ --n ] = '\0';
   if ( !n ) snprintf( buf, sizeof buf, "error %lu", (unsigned long) e );
   return buf;
}

/* ------------------------------------------------------------------ */
/* which CPU                                                            */
/* ------------------------------------------------------------------ */

static struct
{
   char     vendor[ 13 ];
   unsigned family, model;
   char     brand[ 49 ];
} cpu;

static void cpu_identify( void )
{
   unsigned a, b, c, d, max_ext;

   __cpuid( 0, a, b, c, d );
   memcpy( cpu.vendor, &b, 4 );
   memcpy( cpu.vendor + 4, &d, 4 );
   memcpy( cpu.vendor + 8, &c, 4 );
   cpu.vendor[ 12 ] = '\0';

   __cpuid( 1, a, b, c, d );
   cpu.family = ( a >> 8 ) & 0xF;
   cpu.model  = ( a >> 4 ) & 0xF;
   if ( cpu.family == 0xF || cpu.family == 0x6 )
      cpu.model |= ( ( a >> 12 ) & 0xF0 );
   if ( cpu.family == 0xF )
      cpu.family += ( a >> 20 ) & 0xFF;

   __cpuid( 0x80000000u, max_ext, b, c, d );
   if ( max_ext >= 0x80000004u )
   {
      unsigned i, *p = (unsigned *) cpu.brand;

      for ( i = 0; i < 3; i++ )
      {
         __cpuid( 0x80000002u + i, a, b, c, d );
         p[ 4 * i ] = a; p[ 4 * i + 1 ] = b; p[ 4 * i + 2 ] = c; p[ 4 * i + 3 ] = d;
      }
      cpu.brand[ 48 ] = '\0';
   }
   /* trimmed: the string is padded with spaces to 48 */
   {
      char *s = cpu.brand, *e;

      while ( *s == ' ' ) s++;
      memmove( cpu.brand, s, strlen( s ) + 1 );
      for ( e = cpu.brand + strlen( cpu.brand ); e > cpu.brand && e[-1] == ' '; e-- )
         e[-1] = '\0';
   }
}

/* ------------------------------------------------------------------ */
/* PawnIO, over its device                                              */
/* ------------------------------------------------------------------ */

/* The IOCTL interface rather than PawnIOLib.dll, because nothing records
 * where that DLL is installed and the interface is three numbers: a
 * function name padded to 32 bytes followed by 64-bit input cells, and
 * 64-bit output cells back. These codes are the ones LibreHardwareMonitor
 * uses. */
#define PIO_DEVICE_TYPE   ( 41394u << 16 )
#define IOCTL_PIO_LOAD    ( PIO_DEVICE_TYPE | ( 0x821u << 2 ) )
#define IOCTL_PIO_EXECUTE ( PIO_DEVICE_TYPE | ( 0x841u << 2 ) )
#define PIO_NAME_LEN      32

static HANDLE pio = INVALID_HANDLE_VALUE;
static HANDLE pci_mutex;   /* Global\Access_PCI, shared with every tool that pokes SMN */

static bool pio_open( char *why, size_t n )
{
   pio = CreateFileW( L"\\\\?\\GLOBALROOT\\Device\\PawnIO",
                      GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                      0, NULL );
   if ( pio == INVALID_HANDLE_VALUE )
   {
      DWORD e = GetLastError();

      if ( e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND )
         snprintf( why, n, "PawnIO is not installed; it is a signed driver from "
                   "https://pawnio.eu, and LibreHardwareMonitor installs it too" );
      else if ( e == ERROR_ACCESS_DENIED )
         snprintf( why, n, "PawnIO refused to open: only administrators may, "
                   "and this is not running as one" );
      else
         snprintf( why, n, "PawnIO did not open: %s", win_err( e ) );
      return false;
   }
   return true;
}

static void pio_close( void )
{
   if ( pio != INVALID_HANDLE_VALUE ) CloseHandle( pio );
   pio = INVALID_HANDLE_VALUE;
}

/* A module, from the file beside the exe. */
static bool pio_load( const char *file, char *why, size_t n )
{
   char path[ MAX_PATH + 64 ];
   HANDLE f;
   DWORD size, got, ret;
   unsigned char *blob;
   bool ok;

   snprintf( path, sizeof path, "%s%s", exe_dir, file );
   f = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    0, NULL );
   if ( f == INVALID_HANDLE_VALUE )
   {
      snprintf( why, n, "no %s beside avian-sensors.exe; it is the signed "
                "module from https://github.com/namazso/PawnIO.Modules/releases",
                file );
      return false;
   }
   size = GetFileSize( f, NULL );
   if ( size == INVALID_FILE_SIZE || size > ( 4u << 20 ) || !size )
   {
      CloseHandle( f );
      snprintf( why, n, "%s is not a module", file );
      return false;
   }
   blob = malloc( size );
   ok = blob && ReadFile( f, blob, size, &got, NULL ) && got == size;
   CloseHandle( f );
   if ( ok )
      ok = DeviceIoControl( pio, IOCTL_PIO_LOAD, blob, size, NULL, 0, &ret, NULL );
   if ( !ok )
      snprintf( why, n, "PawnIO would not load %s: %s; the driver runs only "
                "the signed builds, unmodified", file, win_err( GetLastError() ) );
   free( blob );
   return ok;
}

static bool pio_exec( const char *fn, const uint64_t *in, size_t nin,
                      uint64_t *out, size_t nout )
{
   unsigned char buf[ PIO_NAME_LEN + 8 * sizeof( uint64_t ) ];
   DWORD ret = 0;

   if ( nin > 8 || strlen( fn ) >= PIO_NAME_LEN ) return false;
   memset( buf, 0, sizeof buf );
   memcpy( buf, fn, strlen( fn ) );
   if ( nin ) memcpy( buf + PIO_NAME_LEN, in, nin * sizeof *in );
   if ( !DeviceIoControl( pio, IOCTL_PIO_EXECUTE, buf,
                          (DWORD) ( PIO_NAME_LEN + nin * sizeof *in ),
                          out, (DWORD) ( nout * sizeof *out ), &ret, NULL ) )
      return false;
   return ret == nout * sizeof *out;
}

static bool read_msr( uint32_t msr, uint64_t *value )
{
   uint64_t in = msr;

   return pio_exec( "ioctl_read_msr", &in, 1, value, 1 );
}

/* SMN is reached through one index/data register pair that every tool on
 * the machine shares, so the read is bracketed by the mutex they all take.
 * Ten milliseconds, as LibreHardwareMonitor waits; a reading skipped is
 * better than two tools tangling. */
static bool read_smn( uint32_t offset, uint32_t *value )
{
   uint64_t in = offset, out = 0;
   bool held = false, ok;

   if ( !pci_mutex ) pci_mutex = CreateMutexA( NULL, FALSE, "Global\\Access_PCI" );
   if ( pci_mutex )
   {
      DWORD w = WaitForSingleObject( pci_mutex, 10 );

      if ( w == WAIT_OBJECT_0 || w == WAIT_ABANDONED ) held = true;
      else return false;
   }
   ok = pio_exec( "ioctl_read_smn", &in, 1, &out, 1 );
   if ( held ) ReleaseMutex( pci_mutex );
   if ( ok ) *value = (uint32_t) out;
   return ok;
}

/* ------------------------------------------------------------------ */
/* energy counters, into watts                                          */
/* ------------------------------------------------------------------ */

/* Both vendors count energy in a 32-bit register that wraps, in units the
 * CPU states. Watts is the difference between two readings over the time
 * between them, so the first call has nothing to say. */
struct energy
{
   double   joules_per_count;
   uint32_t last;
   uint64_t last_ms;
   bool     have;
   float    watts;
};

static float energy_watts( struct energy *e, uint32_t count, uint64_t now_ms )
{
   if ( e->have && now_ms > e->last_ms )
   {
      uint32_t delta = count - e->last;   /* unsigned, so the wrap is free */
      double seconds = ( now_ms - e->last_ms ) / 1000.0;

      if ( seconds < 0.5 ) return e->watts;
      e->watts = (float) ( delta * e->joules_per_count / seconds );
   }
   e->last = count;
   e->last_ms = now_ms;
   e->have = true;
   return e->watts;
}

/* ------------------------------------------------------------------ */
/* AMD, Zen                                                             */
/* ------------------------------------------------------------------ */

/* Register offsets and decoding from LibreHardwareMonitor's Amd17Cpu.cs,
 * which follows Linux's k10temp. Temperatures come over SMN; energy from
 * MSRs the module allows. */
#define AMD_THM_TCON_CUR_TMP   0x00059800u
#define AMD_CCD_TEMP_ZEN2_3    0x00059954u   /* family 17h models 31h/71h, 19h model 21h */
#define AMD_CCD_TEMP_ZEN4_5    0x00059b08u   /* model 61h (Raphael) and 44h (Granite Ridge) */
#define AMD_CCD_TEMP_TURIN     0x000599F0u   /* family 1Ah models up to 2Fh */
#define AMD_TEMP_RANGE_SEL     0x80000u
#define AMD_TEMP_TJ_SEL        0x30000u
#define AMD_CCD_VALID          0x800u
#define AMD_MSR_PWR_UNIT       0xC0010299u
#define AMD_MSR_PKG_ENERGY     0xC001029Bu

static struct
{
   bool          known;
   uint32_t      ccd_base;
   int           nccd;
   bool          ccd_valid_bit;   /* Turin says present with a bit, others with non-zero */
   float         offset;          /* Tctl to Tdie on the first Zen parts */
   struct energy energy;
} amd;

static bool amd_setup( char *why, size_t n )
{
   uint64_t v;

   memset( &amd, 0, sizeof amd );
   if ( cpu.family < 0x17 || cpu.family > 0x1A )
   {
      snprintf( why, n, "AMD family %Xh is not one this knows (Zen is 17h to "
                "1Ah)", cpu.family );
      return false;
   }

   /* Which parts have per-CCD registers, and where. The list is
    * LibreHardwareMonitor's; a model not on it gets Tctl alone. */
   if ( cpu.family == 0x1A && cpu.model <= 0x2F )
   {
      amd.ccd_base = AMD_CCD_TEMP_TURIN;
      amd.nccd = 16;
      amd.ccd_valid_bit = true;
   }
   else if ( cpu.model == 0x61 || cpu.model == 0x44 )
   {
      amd.ccd_base = AMD_CCD_TEMP_ZEN4_5;
      amd.nccd = 8;
   }
   else if ( cpu.model == 0x31 || cpu.model == 0x71 || cpu.model == 0x21 )
   {
      amd.ccd_base = AMD_CCD_TEMP_ZEN2_3;
      amd.nccd = 8;
   }

   /* The first Zen desktop parts report Tctl with a fixed offset above the
    * die temperature; k10temp's table, as LibreHardwareMonitor copied it. */
   if ( strstr( cpu.brand, "1600X" ) || strstr( cpu.brand, "1700X" )
        || strstr( cpu.brand, "1800X" ) )
      amd.offset = -20.0f;
   else if ( strstr( cpu.brand, "Threadripper 19" )
             || strstr( cpu.brand, "Threadripper 29" ) )
      amd.offset = -27.0f;
   else if ( strstr( cpu.brand, "2700X" ) )
      amd.offset = -10.0f;

   /* ESU, bits 12:8: energy counts in 1/2^ESU joules, 15.3 uJ at the
    * usual 16. */
   if ( read_msr( AMD_MSR_PWR_UNIT, &v ) )
      amd.energy.joules_per_count = pow( 0.5, (int) ( ( v >> 8 ) & 0x1F ) );
   amd.known = true;
   return true;
}

static void amd_sample( struct avsens_snapshot *s, uint64_t now_ms )
{
   uint32_t r;
   uint64_t v;
   int i;

   if ( read_smn( AMD_THM_TCON_CUR_TMP, &r ) )
   {
      /* Bits 31:21 in eighths of a degree; RANGE_SEL, or TJ_SEL both set
       * on newer parts, means the scale starts at -49. */
      bool shifted = ( r & AMD_TEMP_RANGE_SEL )
                     || ( r & AMD_TEMP_TJ_SEL ) == AMD_TEMP_TJ_SEL;
      float t = ( r >> 21 ) * 0.125f;

      if ( shifted ) t -= 49.0f;
      s->temp = t + amd.offset;
   }

   for ( i = 0; i < amd.nccd && s->nzones < AVSENS_ZONES; i++ )
   {
      bool present;
      float t;

      if ( !read_smn( amd.ccd_base + 4 * i, &r ) ) continue;
      present = amd.ccd_valid_bit ? ( r & AMD_CCD_VALID ) != 0 : ( r & 0xFFF ) > 0;
      t = ( r & 0xFFF ) * 0.125f - 305.0f;
      if ( !present || t >= 125.0f ) continue;
      snprintf( s->zone[ s->nzones ].label, sizeof s->zone[0].label, "CCD%d", i + 1 );
      s->zone[ s->nzones ].temp = t;
      s->nzones++;
   }

   if ( amd.energy.joules_per_count > 0 && read_msr( AMD_MSR_PKG_ENERGY, &v ) )
      s->power = energy_watts( &amd.energy, (uint32_t) v, now_ms );
}

/* ------------------------------------------------------------------ */
/* Intel, Core                                                          */
/* ------------------------------------------------------------------ */

/* From LibreHardwareMonitor's IntelCpu.cs, which follows coretemp. All
 * package-scoped, so it does not matter which core answers. */
#define INTEL_IA32_PACKAGE_THERM_STATUS 0x1B1u
#define INTEL_IA32_TEMPERATURE_TARGET   0x1A2u
#define INTEL_MSR_RAPL_POWER_UNIT       0x606u
#define INTEL_MSR_PKG_ENERGY_STATUS     0x611u

static struct
{
   bool          known;
   float         tjmax;
   struct energy energy;
} intel;

static bool intel_setup( char *why, size_t n )
{
   uint64_t v;

   memset( &intel, 0, sizeof intel );
   if ( cpu.family != 6 )
   {
      snprintf( why, n, "Intel family %u is not one this knows (Core is 6)",
                cpu.family );
      return false;
   }
   /* TjMax, bits 23:16, the point the delta below counts down from; 100
    * when the CPU does not say. */
   intel.tjmax = 100.0f;
   if ( read_msr( INTEL_IA32_TEMPERATURE_TARGET, &v ) && ( ( v >> 16 ) & 0xFF ) )
      intel.tjmax = (float) ( ( v >> 16 ) & 0xFF );

   if ( read_msr( INTEL_MSR_RAPL_POWER_UNIT, &v ) )
   {
      int esu = (int) ( ( v >> 8 ) & 0x1F );

      /* Silvermont and Airmont Atoms state the unit the other way up. */
      if ( cpu.model == 0x37 || cpu.model == 0x4A || cpu.model == 0x4D
           || cpu.model == 0x5A || cpu.model == 0x5D || cpu.model == 0x4C )
         intel.energy.joules_per_count = 1.0e-6 * ( 1u << esu );
      else
         intel.energy.joules_per_count = 1.0 / ( 1u << esu );
   }
   intel.known = true;
   return true;
}

static void intel_sample( struct avsens_snapshot *s, uint64_t now_ms )
{
   uint64_t v;

   /* Bit 31 says the reading is valid; bits 22:16 are degrees below TjMax. */
   if ( read_msr( INTEL_IA32_PACKAGE_THERM_STATUS, &v ) && ( v & 0x80000000u ) )
      s->temp = intel.tjmax - (float) ( ( v >> 16 ) & 0x7F );

   if ( intel.energy.joules_per_count > 0
        && read_msr( INTEL_MSR_PKG_ENERGY_STATUS, &v ) )
      s->power = energy_watts( &intel.energy, (uint32_t) v, now_ms );
}

/* ------------------------------------------------------------------ */
/* the backend, whichever it is                                         */
/* ------------------------------------------------------------------ */

static bool backend_ready;
static uint64_t backend_retry_at;
static char backend_name[ 32 ];
static char last_why[ 256 ];

static void fake_sample( struct avsens_snapshot *s, uint64_t now_ms )
{
   float phase = (float) ( ( now_ms / 1000 ) % 60 ) / 60.0f * 6.2832f;

   s->temp = 62.0f + 6.0f * sinf( phase );
   s->power = 118.0f + 10.0f * cosf( phase );
   s->nzones = 2;
   snprintf( s->zone[0].label, sizeof s->zone[0].label, "CCD1" );
   s->zone[0].temp = s->temp + 2.0f;
   snprintf( s->zone[1].label, sizeof s->zone[1].label, "CCD2" );
   s->zone[1].temp = s->temp - 4.0f;
}

static bool backend_open( char *why, size_t n )
{
   if ( fake_mode )
   {
      snprintf( backend_name, sizeof backend_name, "fake" );
      return true;
   }
   if ( !pio_open( why, n ) ) return false;

   if ( !strcmp( cpu.vendor, "AuthenticAMD" ) )
   {
      if ( !pio_load( "AMDFamily17.bin", why, n ) || !amd_setup( why, n ) )
      {
         pio_close();
         return false;
      }
      snprintf( backend_name, sizeof backend_name, "pawnio/amd" );
   }
   else if ( !strcmp( cpu.vendor, "GenuineIntel" ) )
   {
      if ( !pio_load( "IntelMSR.bin", why, n ) || !intel_setup( why, n ) )
      {
         pio_close();
         return false;
      }
      snprintf( backend_name, sizeof backend_name, "pawnio/intel" );
   }
   else
   {
      pio_close();
      snprintf( why, n, "CPU vendor \"%s\" is not one this knows", cpu.vendor );
      return false;
   }
   return true;
}

static void backend_sample( struct avsens_snapshot *s, uint64_t now_ms )
{
   if ( fake_mode ) fake_sample( s, now_ms );
   else if ( amd.known ) amd_sample( s, now_ms );
   else if ( intel.known ) intel_sample( s, now_ms );
}

/* ------------------------------------------------------------------ */
/* the sampler                                                          */
/* ------------------------------------------------------------------ */

static DWORD WINAPI sampler( LPVOID unused )
{
   (void) unused;
   for ( ;; )
   {
      struct avsens_snapshot t;
      uint64_t now = GetTickCount64();

      if ( !backend_ready && now >= backend_retry_at )
      {
         char why[ 256 ] = "";

         backend_ready = backend_open( why, sizeof why );
         if ( backend_ready )
         {
            say( "reading the %s through %s", cpu.brand[0] ? cpu.brand : cpu.vendor,
                 backend_name );
            last_why[0] = '\0';
         }
         else
         {
            if ( strcmp( why, last_why ) )
               say( "no readings: %s. Trying again every %d seconds", why,
                    RETRY_MS / 1000 );
            snprintf( last_why, sizeof last_why, "%s", why );
            backend_retry_at = now + RETRY_MS;
         }
      }

      memset( &t, 0, sizeof t );
      t.magic = AVSENS_MAGIC;
      t.version = AVSENS_VERSION;
      t.size = sizeof t;
      t.temp = t.power = -1.0f;
      snprintf( t.source, sizeof t.source, "%s", backend_ready ? backend_name : "" );
      snprintf( t.cpu, sizeof t.cpu, "%.47s", cpu.brand );
      if ( backend_ready ) backend_sample( &t, now );
      t.sampled_ms = now;

      EnterCriticalSection( &lock );
      snap = t;
      LeaveCriticalSection( &lock );

      if ( WaitForSingleObject( stop_event, 1000 ) == WAIT_OBJECT_0 ) break;
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* the pipe                                                             */
/* ------------------------------------------------------------------ */

/* Outbound only, one message per connection: whoever connects gets the
 * latest snapshot and is disconnected. Readable by everyone on the
 * machine, which is the point; writable by nobody, since nothing is read
 * from it. Overlapped, so stopping the service can interrupt the wait. */
static DWORD WINAPI pipe_server( LPVOID unused )
{
   SECURITY_ATTRIBUTES sa;
   PSECURITY_DESCRIPTOR sd = NULL;
   HANDLE connected = CreateEventA( NULL, TRUE, FALSE, NULL );

   (void) unused;
   if ( !ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GR;;;WD)(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, NULL ) )
   {
      say( "could not build the pipe's permissions: %s", win_err( GetLastError() ) );
      return 1;
   }
   sa.nLength = sizeof sa;
   sa.lpSecurityDescriptor = sd;
   sa.bInheritHandle = FALSE;

   for ( ;; )
   {
      HANDLE h, waits[2];
      OVERLAPPED ov;
      DWORD w, written;
      struct avsens_snapshot copy;

      h = CreateNamedPipeA( AVSENS_PIPE,
                            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT
                            | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, sizeof snap, 0, 0, &sa );
      if ( h == INVALID_HANDLE_VALUE )
      {
         say( "could not create the pipe: %s", win_err( GetLastError() ) );
         if ( WaitForSingleObject( stop_event, 5000 ) == WAIT_OBJECT_0 ) break;
         continue;
      }

      memset( &ov, 0, sizeof ov );
      ov.hEvent = connected;
      ResetEvent( connected );
      if ( !ConnectNamedPipe( h, &ov ) )
      {
         DWORD e = GetLastError();

         if ( e == ERROR_IO_PENDING )
         {
            waits[0] = stop_event;
            waits[1] = connected;
            w = WaitForMultipleObjects( 2, waits, FALSE, INFINITE );
            if ( w == WAIT_OBJECT_0 )
            {
               CancelIo( h );
               CloseHandle( h );
               break;
            }
         }
         else if ( e != ERROR_PIPE_CONNECTED )
         {
            CloseHandle( h );
            if ( WaitForSingleObject( stop_event, 1000 ) == WAIT_OBJECT_0 ) break;
            continue;
         }
      }

      EnterCriticalSection( &lock );
      copy = snap;
      LeaveCriticalSection( &lock );

      memset( &ov, 0, sizeof ov );
      ov.hEvent = connected;
      ResetEvent( connected );
      if ( !WriteFile( h, &copy, sizeof copy, &written, &ov )
           && GetLastError() == ERROR_IO_PENDING )
         GetOverlappedResult( h, &ov, &written, TRUE );
      FlushFileBuffers( h );
      DisconnectNamedPipe( h );
      CloseHandle( h );
   }
   LocalFree( sd );
   CloseHandle( connected );
   return 0;
}

/* ------------------------------------------------------------------ */
/* running, as a service or in a window                                 */
/* ------------------------------------------------------------------ */

static int run( void )
{
   HANDLE threads[2];

   InitializeCriticalSection( &lock );
   cpu_identify();
   say( "avian-sensors starting: %s (%s family %Xh model %Xh)%s",
        cpu.brand[0] ? cpu.brand : "unknown CPU", cpu.vendor, cpu.family,
        cpu.model, fake_mode ? ", inventing readings" : "" );

   threads[0] = CreateThread( NULL, 0, sampler, NULL, 0, NULL );
   threads[1] = CreateThread( NULL, 0, pipe_server, NULL, 0, NULL );
   WaitForSingleObject( stop_event, INFINITE );
   WaitForMultipleObjects( 2, threads, TRUE, 5000 );
   CloseHandle( threads[0] );
   CloseHandle( threads[1] );
   pio_close();
   say( "avian-sensors stopped" );
   return 0;
}

static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status;

static void set_state( DWORD state )
{
   status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
   status.dwCurrentState = state;
   status.dwControlsAccepted = state == SERVICE_RUNNING
                               ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
   status.dwWaitHint = 5000;
   SetServiceStatus( status_handle, &status );
}

static void WINAPI handler( DWORD control )
{
   if ( control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN )
   {
      set_state( SERVICE_STOP_PENDING );
      SetEvent( stop_event );
   }
}

static void WINAPI service_main( DWORD argc, LPSTR *argv )
{
   DWORD i;

   for ( i = 1; i < argc; i++ )
      if ( !strcmp( argv[i], "--fake" ) ) fake_mode = true;
   status_handle = RegisterServiceCtrlHandlerA( SERVICE_NAME, handler );
   if ( !status_handle ) return;
   set_state( SERVICE_START_PENDING );
   set_state( SERVICE_RUNNING );
   run();
   set_state( SERVICE_STOPPED );
}

static BOOL WINAPI console_break( DWORD type )
{
   (void) type;
   SetEvent( stop_event );
   return TRUE;
}

/* ------------------------------------------------------------------ */
/* installing                                                           */
/* ------------------------------------------------------------------ */

static SC_HANDLE open_scm( void )
{
   SC_HANDLE scm = OpenSCManagerA( NULL, NULL, SC_MANAGER_CREATE_SERVICE );

   if ( !scm )
   {
      DWORD e = GetLastError();

      if ( e == ERROR_ACCESS_DENIED )
         fprintf( stderr, "Installing a service needs an administrator prompt: "
                  "right-click Command Prompt or PowerShell, Run as administrator, "
                  "and run this again.\n" );
      else
         fprintf( stderr, "Could not reach the service manager: %s\n", win_err( e ) );
   }
   return scm;
}

static int install( void )
{
   char exe[ MAX_PATH ], cmd[ MAX_PATH + 32 ];
   SC_HANDLE scm, svc;
   SERVICE_DESCRIPTIONA desc;
   static char text[] = "Reads the CPU's temperature and power through PawnIO "
                        "and hands them to avian-miner over a named pipe.";

   GetModuleFileNameA( NULL, exe, sizeof exe );
   snprintf( cmd, sizeof cmd, "\"%s\"%s", exe, fake_mode ? " --fake" : "" );

   scm = open_scm();
   if ( !scm ) return 1;
   svc = CreateServiceA( scm, SERVICE_NAME, SERVICE_DISPLAY, SERVICE_ALL_ACCESS,
                         SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                         SERVICE_ERROR_NORMAL, cmd, NULL, NULL, NULL, NULL, NULL );
   if ( !svc )
   {
      DWORD e = GetLastError();

      if ( e == ERROR_SERVICE_EXISTS )
         fprintf( stderr, "%s is already installed; --uninstall first to move "
                  "or change it.\n", SERVICE_NAME );
      else
         fprintf( stderr, "Could not install: %s\n", win_err( e ) );
      CloseServiceHandle( scm );
      return 1;
   }
   desc.lpDescription = text;
   ChangeServiceConfig2A( svc, SERVICE_CONFIG_DESCRIPTION, &desc );
   if ( !StartServiceA( svc, 0, NULL ) )
      fprintf( stderr, "Installed, but it did not start: %s\n",
               win_err( GetLastError() ) );
   else
      printf( "Installed and started %s, running %s at every boot.\n"
              "The miner finds it within half a minute; its log is "
              "%%ProgramData%%\\avian-miner\\avian-sensors.log.\n", SERVICE_NAME, cmd );
   CloseServiceHandle( svc );
   CloseServiceHandle( scm );
   return 0;
}

static int uninstall( void )
{
   SC_HANDLE scm, svc;
   SERVICE_STATUS st;

   scm = open_scm();
   if ( !scm ) return 1;
   svc = OpenServiceA( scm, SERVICE_NAME, SERVICE_STOP | DELETE );
   if ( !svc )
   {
      fprintf( stderr, "%s is not installed.\n", SERVICE_NAME );
      CloseServiceHandle( scm );
      return 1;
   }
   ControlService( svc, SERVICE_CONTROL_STOP, &st );
   if ( !DeleteService( svc ) )
      fprintf( stderr, "Could not remove it: %s\n", win_err( GetLastError() ) );
   else
      printf( "Removed %s.\n", SERVICE_NAME );
   CloseServiceHandle( svc );
   CloseServiceHandle( scm );
   return 0;
}

/* ------------------------------------------------------------------ */

static void usage( void )
{
   printf( "avian-sensors: CPU temperature and power for avian-miner on Windows.\n\n"
           "  --install     register and start the service (administrator prompt)\n"
           "  --uninstall   stop and remove it (administrator prompt)\n"
           "  --console     run in this window instead of as a service\n"
           "  --fake        with --install or --console: invent readings, to test\n\n"
           "Needs PawnIO (https://pawnio.eu) and, beside this exe, AMDFamily17.bin\n"
           "or IntelMSR.bin from https://github.com/namazso/PawnIO.Modules/releases.\n" );
}

int main( int argc, char **argv )
{
   int i, mode = 0;
   char *slash;

   GetModuleFileNameA( NULL, exe_dir, sizeof exe_dir );
   slash = strrchr( exe_dir, '\\' );
   if ( slash ) slash[1] = '\0';

   for ( i = 1; i < argc; i++ )
   {
      if ( !strcmp( argv[i], "--install" ) ) mode = 1;
      else if ( !strcmp( argv[i], "--uninstall" ) ) mode = 2;
      else if ( !strcmp( argv[i], "--console" ) ) mode = 3;
      else if ( !strcmp( argv[i], "--fake" ) ) fake_mode = true;
      else { usage(); return argv[i][0] == '-' && argv[i][1] == '-' && !strcmp( argv[i], "--help" ) ? 0 : 1; }
   }

   stop_event = CreateEventA( NULL, TRUE, FALSE, NULL );
   if ( mode == 1 ) return install();
   if ( mode == 2 ) return uninstall();
   if ( mode == 3 )
   {
      console_mode = true;
      SetConsoleCtrlHandler( console_break, TRUE );
      fprintf( stderr, "Running in this window; Ctrl-C stops it.\n" );
      return run();
   }

   /* No argument: the service manager is starting us, or someone double-
    * clicked. The dispatcher tells the two apart. */
   {
      static SERVICE_TABLE_ENTRYA table[] = {
         { (LPSTR) SERVICE_NAME, service_main }, { NULL, NULL } };
      char path[ MAX_PATH + 32 ];
      const char *pd = getenv( "ProgramData" );

      /* ProgramData, because Program Files is read-only to the person who
       * wants to read the log; beside the exe when that fails, which is a
       * folder someone unzipped. */
      if ( pd )
      {
         snprintf( path, sizeof path, "%.200s\\avian-miner", pd );
         CreateDirectoryA( path, NULL );
         snprintf( path, sizeof path, "%.200s\\avian-miner\\avian-sensors.log", pd );
         log_file = fopen( path, "a" );
      }
      if ( !log_file )
      {
         snprintf( path, sizeof path, "%savian-sensors.log", exe_dir );
         log_file = fopen( path, "a" );
      }
      if ( !StartServiceCtrlDispatcherA( table ) )
      {
         if ( GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT )
         {
            usage();
            return 1;
         }
         fprintf( stderr, "service start failed: %s\n", win_err( GetLastError() ) );
         return 1;
      }
   }
   return 0;
}

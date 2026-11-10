/*
 * Which CPUs the miner may pin threads to, and how the machine is laid out.
 *
 * Copyright 2026 The avian-miner developers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * Nothing from miner.h is used, so this compiles and runs on its own, which
 * is how it was tested.
 */

#include "cpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* ------------------------------------------------------------------ */
/* the set                                                              */
/* ------------------------------------------------------------------ */

static unsigned char allowed[ CPUS_MAX ];
static bool restricted;   /* false until cpus_parse succeeds: everything goes */

bool cpus_allowed( int cpu )
{
   if ( cpu < 0 || cpu >= CPUS_MAX ) return false;
   return !restricted || allowed[ cpu ];
}

int cpus_count( int num_cpus )
{
   int i, n = 0;

   if ( num_cpus > CPUS_MAX ) num_cpus = CPUS_MAX;
   for ( i = 0; i < num_cpus; i++ ) n += cpus_allowed( i );
   return n;
}

void cpus_format( char *out, size_t n, int num_cpus )
{
   size_t i;

   if ( !n ) return;
   for ( i = 0; i < n - 1 && (int) i < num_cpus; i++ )
      out[i] = cpus_allowed( (int) i ) ? '!' : '.';
   out[i] = '\0';
}

/* One decimal CPU number, advancing *s past it. */
static bool take_number( const char **s, int *out )
{
   const char *p = *s;
   long v = 0;

   if ( !isdigit( (unsigned char) *p ) ) return false;
   while ( isdigit( (unsigned char) *p ) )
   {
      v = v * 10 + ( *p - '0' );
      if ( v > CPUS_MAX ) v = CPUS_MAX;   /* keep it finite; rejected below */
      p++;
   }
   *out = (int) v;
   *s = p;
   return true;
}

bool cpus_parse( const char *arg, int num_cpus, char *err, size_t err_len )
{
   unsigned char set[ CPUS_MAX ];
   const char *p = arg;
   int i, n = 0;

   memset( set, 0, sizeof set );
   if ( num_cpus > CPUS_MAX ) num_cpus = CPUS_MAX;
   while ( isspace( (unsigned char) *p ) ) p++;

   if ( p[0] == '0' && ( p[1] == 'x' || p[1] == 'X' ) )
   {
      /* The mask, as it always was. Bits above the CPUs this machine has
       * are ignored rather than refused, because 0xffffffff on an 8-CPU
       * box has always meant "all of them". */
      char *end;
      unsigned long long mask = strtoull( p, &end, 16 );

      if ( end == p + 2 || *end )
      {
         snprintf( err, err_len, "--cpu-affinity %s: not a hex mask", arg );
         return false;
      }
      for ( i = 0; i < 64 && i < num_cpus; i++ )
         if ( ( mask >> i ) & 1 ) set[i] = 1;
   }
   else
   {
      /* A list: "0-21" or "0-11,16-23". A bare decimal used to be read as a
       * mask too, so "255" meant eight CPUs; it now means CPU 255, and on a
       * machine without one the error below says what to write instead. */
      for ( ;; )
      {
         int lo, hi;

         if ( !take_number( &p, &lo ) )
         {
            snprintf( err, err_len, "--cpu-affinity %s: expected a CPU number "
                      "at \"%.12s\"; the form is 0-21 or 0-11,16-23, or a hex "
                      "mask such as 0x3fffff", arg, p );
            return false;
         }
         hi = lo;
         if ( *p == '-' )
         {
            p++;
            if ( !take_number( &p, &hi ) || hi < lo )
            {
               snprintf( err, err_len, "--cpu-affinity %s: a range is "
                         "low-high, as in 0-21", arg );
               return false;
            }
         }
         if ( hi >= num_cpus )
         {
            snprintf( err, err_len, "--cpu-affinity %s: this machine has %d "
                      "CPUs, numbered 0 to %d%s", arg, num_cpus, num_cpus - 1,
                      hi > 99 && num_cpus <= 64
                      ? "; a mask is written with 0x in front" : "" );
            return false;
         }
         for ( i = lo; i <= hi; i++ ) set[i] = 1;
         if ( *p == ',' ) { p++; continue; }
         while ( isspace( (unsigned char) *p ) ) p++;
         if ( !*p ) break;
         snprintf( err, err_len, "--cpu-affinity %s: unexpected \"%.12s\"",
                   arg, p );
         return false;
      }
   }

   for ( i = 0; i < num_cpus; i++ ) n += set[i];
   if ( !n )
   {
      snprintf( err, err_len, "--cpu-affinity %s: names none of this "
                "machine's %d CPUs", arg, num_cpus );
      return false;
   }
   memcpy( allowed, set, sizeof allowed );
   restricted = true;
   return true;
}

/* ------------------------------------------------------------------ */
/* the layout                                                           */
/* ------------------------------------------------------------------ */

/* core_of[cpu] is a small integer shared by the CPUs that are one core's
 * SMT siblings, or -1 where the layout could not be read. */
static int core_of[ CPUS_MAX ];

#if defined(_WIN32)

static int group_base( WORD group )
{
   int base = 0;
   WORD g;

   for ( g = 0; g < group; g++ ) base += GetActiveProcessorCount( g );
   return base;
}

static void read_layout( int num_cpus )
{
   DWORD len = 0;
   char *buf, *p;
   int core = 0;

   GetLogicalProcessorInformationEx( RelationProcessorCore, NULL, &len );
   if ( !len ) return;
   buf = malloc( len );
   if ( !buf ) return;
   if ( !GetLogicalProcessorInformationEx( RelationProcessorCore,
                                           (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) buf,
                                           &len ) )
   {
      free( buf );
      return;
   }
   for ( p = buf; p < buf + len; )
   {
      PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info =
         (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) p;
      WORD g;

      if ( info->Relationship == RelationProcessorCore )
      {
         for ( g = 0; g < info->Processor.GroupCount; g++ )
         {
            int base = group_base( info->Processor.GroupMask[g].Group );
            KAFFINITY mask = info->Processor.GroupMask[g].Mask;
            int bit;

            for ( bit = 0; bit < (int) ( 8 * sizeof mask ); bit++ )
               if ( ( mask >> bit ) & 1 && base + bit < num_cpus )
                  core_of[ base + bit ] = core;
         }
         core++;
      }
      p += info->Size;
   }
   free( buf );
}

#else

/* "22-23" or "22,23" or "5": the lowest CPU named, which is what names the
 * core, or -1. */
static int first_in_list( const char *s )
{
   int v = -1;

   while ( *s && !isdigit( (unsigned char) *s ) ) s++;
   if ( *s ) v = atoi( s );
   return v;
}

static void read_layout( int num_cpus )
{
   int cpu;

   for ( cpu = 0; cpu < num_cpus; cpu++ )
   {
      static const char *names[] = { "core_cpus_list", "thread_siblings_list" };
      char path[ 128 ], line[ 256 ];
      size_t k;

      for ( k = 0; k < sizeof names / sizeof names[0]; k++ )
      {
         FILE *f;

         snprintf( path, sizeof path,
                   "/sys/devices/system/cpu/cpu%d/topology/%s", cpu, names[k] );
         f = fopen( path, "r" );
         if ( !f ) continue;
         if ( fgets( line, sizeof line, f ) )
         {
            int first = first_in_list( line );
            if ( first >= 0 && first <= cpu ) core_of[ cpu ] = first;
         }
         fclose( f );
         if ( core_of[ cpu ] >= 0 ) break;
      }
   }
}

#endif

/* core_of filled in, once, with each core named after its lowest CPU
 * whatever the platform called it, so "the first CPU of a core" means
 * core_of[i] == i from here on. The layout does not change while the miner
 * runs, and the API asks about it every few seconds, so it is read once.
 * Returns true when every CPU's core is known. */
static bool layout( int num_cpus )
{
   static int raw[ CPUS_MAX ];
   static bool done, complete;
   int i, j;

   if ( done ) return complete;
   done = true;
   if ( num_cpus > CPUS_MAX ) num_cpus = CPUS_MAX;
   for ( i = 0; i < CPUS_MAX; i++ ) core_of[i] = -1;
   read_layout( num_cpus );

   memcpy( raw, core_of, sizeof raw );
   complete = true;
   for ( i = 0; i < num_cpus; i++ )
   {
      if ( raw[i] < 0 ) { complete = false; continue; }
      for ( j = 0; j < i && raw[j] != raw[i]; j++ ) ;
      core_of[i] = j;
   }
   return complete;
}

int cpus_cores( int num_cpus )
{
   int i, cores = 0;

   if ( num_cpus > CPUS_MAX ) num_cpus = CPUS_MAX;
   if ( !layout( num_cpus ) ) return 0;
   for ( i = 0; i < num_cpus; i++ ) cores += core_of[i] == i;
   return cores;
}

/* Write a set of CPUs as the shortest list: 0-21, or 0-11,16-23. */
static void format_ranges( char *out, size_t n, const unsigned char *in,
                           int num_cpus )
{
   size_t used = 0;
   int i = 0;

   out[0] = '\0';
   while ( i < num_cpus )
   {
      int j;

      if ( !in[i] ) { i++; continue; }
      for ( j = i; j + 1 < num_cpus && in[ j + 1 ]; j++ ) ;
      if ( j > i )
         used += snprintf( out + used, used < n ? n - used : 0, "%s%d-%d",
                           used ? "," : "", i, j );
      else
         used += snprintf( out + used, used < n ? n - used : 0, "%s%d",
                           used ? "," : "", i );
      i = j + 1;
   }
}

void cpus_ranges( char *out, size_t n, int num_cpus )
{
   unsigned char set[ CPUS_MAX ];
   int i;

   if ( num_cpus > CPUS_MAX ) num_cpus = CPUS_MAX;
   for ( i = 0; i < num_cpus; i++ ) set[i] = cpus_allowed( i );
   format_ranges( out, n, set, num_cpus );
}

int cpus_list( int num_cpus )
{
   unsigned char gpu_side[ CPUS_MAX ], cpu_side[ CPUS_MAX ];
   char a[ 4096 ], b[ 4096 ];
   int i, cores = 0, known = 0, last_core, per_core = 0;

   if ( num_cpus > CPUS_MAX ) num_cpus = CPUS_MAX;
   layout( num_cpus );

   /* Count cores by their first CPU, and check whether every core has the
    * same number of siblings, which is what lets the summary say "two per
    * core" rather than list them. */
   for ( i = 0; i < num_cpus; i++ )
   {
      int j, first = 1, n = 0;

      if ( core_of[i] < 0 ) continue;
      known++;
      for ( j = 0; j < num_cpus; j++ )
         if ( core_of[j] == core_of[i] ) { n++; if ( j < i ) first = 0; }
      if ( first )
      {
         cores++;
         if ( !per_core ) per_core = n;
         else if ( per_core != n ) per_core = -1;
      }
   }

   if ( known < num_cpus )
   {
      /* Not read, so nothing is known about pairing. Said plainly, and the
       * split below treats every CPU as a core of its own. */
      printf( "%d logical CPUs; how they pair into cores could not be read, "
              "so each is taken as a core of its own.\n", num_cpus );
      for ( i = 0; i < num_cpus; i++ ) core_of[i] = i;
      cores = num_cpus;
      per_core = 1;
   }
   else if ( per_core == 1 )
      printf( "%d CPUs on %d cores, one per core: no SMT.\n", num_cpus, cores );
   else if ( per_core > 1 )
      printf( "%d logical CPUs on %d cores, %d per core.\n", num_cpus, cores,
              per_core );
   else
      printf( "%d logical CPUs on %d cores, not the same number per core.\n",
              num_cpus, cores );

   if ( per_core != 1 || known < num_cpus )
   {
      /* One line per core, because with SMT the question that brings
       * anyone here is which CPUs share one. */
      int c, shown = 0;

      for ( c = 0; c < num_cpus; c++ )
      {
         unsigned char sib[ CPUS_MAX ];
         int any = 0;

         if ( core_of[c] != c ) continue;   /* only the first of each core */
         memset( sib, 0, sizeof sib );
         for ( i = 0; i < num_cpus; i++ )
            if ( core_of[i] == c ) { sib[i] = 1; any++; }
         if ( any < 2 && per_core != -1 ) continue;
         format_ranges( a, sizeof a, sib, num_cpus );
         printf( "  core %*d: CPUs %s\n", cores > 99 ? 3 : cores > 9 ? 2 : 1,
                 shown++, a );
      }
   }

   if ( num_cpus < 3 )
   {
      printf( "\nToo few CPUs to split between two miners; run one or the "
              "other.\n" );
      return 0;
   }

   /* The split: the GPU miner's launcher gets the last core, all of its
    * siblings, so the CPU miner never shares a core with it; the CPU miner
    * gets everything else. */
   last_core = core_of[ num_cpus - 1 ];
   memset( gpu_side, 0, sizeof gpu_side );
   memset( cpu_side, 0, sizeof cpu_side );
   for ( i = 0; i < num_cpus; i++ )
   {
      if ( core_of[i] == last_core ) gpu_side[i] = 1;
      else cpu_side[i] = 1;
   }
   format_ranges( a, sizeof a, cpu_side, num_cpus );
   format_ranges( b, sizeof b, gpu_side, num_cpus );

   printf( "\nTo run MinotaurX and a GPU miner together, give the GPU miner "
           "the last core\nand MinotaurX the rest. Without -t it starts one "
           "thread per CPU listed:\n\n"
           "  avian-miner -a minotaurx --cpu-affinity %s ...\n"
           "  avian-miner -a x16rt --gpu --cpu-affinity %s ...\n\n"
           "On Windows, mine-both.bat beside the miner does this.\n", a, b );
   return 0;
}

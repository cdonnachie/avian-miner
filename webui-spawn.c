/*
 * Start avian-webui beside the miner and keep it running.
 *
 * Copyright 2026 The avian-miner developers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * A child process and not a thread, which is the whole point of the thing.
 * The dashboard answers HTTP from a LAN while the miner's own API is a
 * loopback control socket with seturl and quit on it; keeping the two in
 * separate address spaces means a parsing bug in the half that faces a
 * network costs a dashboard rather than hours of hashing, and it stays a
 * property of the arrangement rather than a rule someone has to remember.
 *
 * What this adds is the one thing two processes were missing: something to
 * supervise the second one. HiveOS runs h-run.sh as the miner and has
 * nowhere to put another process, so until now the dashboard was started by
 * hand over ssh and did not survive a reboot.
 *
 * The child is found beside this binary rather than on PATH, the same way
 * the GPU backends are, so a release directory that was copied somewhere
 * still works and a stray avian-webui elsewhere on the system is never the
 * one that runs.
 */

/* miner.h first: it pulls in the compat headers, which define types that
 * MinGW's own pthread.h also defines, and only this order agrees. */
#include "miner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#if defined(WIN32)
#include <windows.h>
#include "compat.h"
#else
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

#define WEBUI_PATH_MAX   1024
#define WEBUI_BACKOFF_MAX 30
/* Up this long counts as having run rather than having failed to start, so
 * a dashboard that dies once an hour is restarted promptly every time
 * instead of inheriting the backoff from the last failure. */
#define WEBUI_SETTLED     60

static char child_path[ WEBUI_PATH_MAX ];
static char child_bind[ 128 ];
static char child_miner[ 128 ];
static volatile bool webui_stopping = false;

#if defined(WIN32)
static HANDLE webui_job   = NULL;
static HANDLE webui_child = NULL;
#else
static volatile pid_t webui_child = -1;
#endif

/* The directory this executable is in, with the trailing separator removed.
 * Nothing portable gives this, so each platform asks the kernel for its own
 * path; argv[0] is not used because it is whatever the caller passed. */
static bool self_dir( char *out, size_t n )
{
   char *cut;

#if defined(WIN32)
   DWORD len = GetModuleFileNameA( NULL, out, (DWORD) n );

   if ( !len || len >= n ) return false;
#else
   ssize_t len = readlink( "/proc/self/exe", out, n - 1 );

   if ( len <= 0 || (size_t) len >= n - 1 ) return false;
   out[ len ] = '\0';
#endif

   cut = strrchr( out, '/' );
#if defined(WIN32)
   {
      char *bs = strrchr( out, '\\' );
      if ( bs && ( !cut || bs > cut ) ) cut = bs;
   }
#endif
   if ( !cut ) return false;
   *cut = '\0';
   return true;
}

#if defined(WIN32)

/* A job object with kill-on-close, so the dashboard cannot outlive the
 * miner however the miner goes away. Windows has no PDEATHSIG, and a child
 * left behind would hold the port against the next run. */
static void webui_job_setup( void )
{
   JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;

   webui_job = CreateJobObject( NULL, NULL );
   if ( !webui_job ) return;

   memset( &lim, 0, sizeof lim );
   lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
   SetInformationJobObject( webui_job, JobObjectExtendedLimitInformation,
                            &lim, sizeof lim );
}

static bool webui_spawn( void )
{
   char cmd[ WEBUI_PATH_MAX + 256 ];
   STARTUPINFOA si;
   PROCESS_INFORMATION pi;

   memset( &si, 0, sizeof si );
   si.cb = sizeof si;

   snprintf( cmd, sizeof cmd, "\"%s\" --bind %s --miner %s",
             child_path, child_bind, child_miner );

   /* Suspended, so it is inside the job before it can run and therefore
    * before it could outlive us by exiting the gap. */
   if ( !CreateProcessA( child_path, cmd, NULL, NULL, FALSE,
                         CREATE_SUSPENDED, NULL, NULL, &si, &pi ) )
   {
      applog( LOG_ERR, "webui: cannot run %s (Windows error %lu)",
              child_path, (unsigned long) GetLastError() );
      return false;
   }

   if ( webui_job ) AssignProcessToJobObject( webui_job, pi.hProcess );
   ResumeThread( pi.hThread );
   CloseHandle( pi.hThread );
   webui_child = pi.hProcess;
   return true;
}

/* Returns the child's exit code, or -1 if it could not be waited for. */
static int webui_wait( void )
{
   DWORD code = 0;

   if ( WaitForSingleObject( webui_child, INFINITE ) != WAIT_OBJECT_0 )
      return -1;
   GetExitCodeProcess( webui_child, &code );
   CloseHandle( webui_child );
   webui_child = NULL;
   return (int) code;
}

static void webui_kill( void )
{
   if ( webui_child ) TerminateProcess( webui_child, 0 );
   if ( webui_job )   CloseHandle( webui_job );
}

#else

static bool webui_spawn( void )
{
   pid_t pid = fork();

   if ( pid < 0 )
   {
      applog( LOG_ERR, "webui: cannot fork: %s", strerror( errno ) );
      return false;
   }

   if ( pid == 0 )
   {
#if defined(__linux__)
      /* So the dashboard goes when the miner does, including when the miner
       * is killed outright and runs no cleanup of its own. */
      prctl( PR_SET_PDEATHSIG, SIGTERM );
#endif
      execl( child_path, child_path, "--bind", child_bind,
             "--miner", child_miner, (char *) NULL );
      _exit( 127 );
   }

   webui_child = pid;
   return true;
}

static int webui_wait( void )
{
   int status = 0;
   pid_t done;

   do { done = waitpid( webui_child, &status, 0 ); }
   while ( done < 0 && errno == EINTR );

   webui_child = -1;
   if ( done < 0 )                 return -1;
   if ( WIFEXITED( status ) )      return WEXITSTATUS( status );
   if ( WIFSIGNALED( status ) )    return 128 + WTERMSIG( status );
   return -1;
}

static void webui_kill( void )
{
   if ( webui_child > 0 ) kill( webui_child, SIGTERM );
}

#endif

static void webui_atexit( void )
{
   webui_stopping = true;
   webui_kill();
}

/* Restarted with a doubling backoff rather than immediately, because the
 * failure that repeats is the one worth not spinning on: a port already
 * taken, which is what a second miner started with --webui on the same
 * machine will hit. Each attempt says why, so the log names the problem
 * instead of scrolling. */
static void *webui_supervisor( void *userdata )
{
   int backoff = 1;

   (void) userdata;

   while ( !webui_stopping )
   {
      time_t started = time( NULL );
      int    code;

      if ( !webui_spawn() )
      {
         if ( webui_stopping ) break;
         sleep( backoff );
         if ( backoff < WEBUI_BACKOFF_MAX ) backoff *= 2;
         continue;
      }

      code = webui_wait();
      if ( webui_stopping ) break;

      if ( time( NULL ) - started >= WEBUI_SETTLED ) backoff = 1;

      applog( LOG_WARNING,
              "webui: the dashboard exited (%d), restarting in %ds",
              code, backoff );
      sleep( backoff );
      if ( backoff < WEBUI_BACKOFF_MAX ) backoff *= 2;
   }

   return NULL;
}

void webui_start( const char *bind, const char *miner_api )
{
   static const char *name =
#if defined(WIN32)
      "avian-webui.exe";
#else
      "avian-webui";
#endif
   char dir[ WEBUI_PATH_MAX ];
   pthread_t tid;

   if ( !self_dir( dir, sizeof dir ) )
   {
      applog( LOG_ERR, "webui: cannot find this binary's own directory, so "
                       "the dashboard was not started" );
      return;
   }

   if ( snprintf( child_path, sizeof child_path, "%s%c%s", dir,
#if defined(WIN32)
                  '\\',
#else
                  '/',
#endif
                  name ) >= (int) sizeof child_path )
   {
      applog( LOG_ERR, "webui: path to %s is too long", name );
      return;
   }

   snprintf( child_bind,  sizeof child_bind,  "%s", bind );
   snprintf( child_miner, sizeof child_miner, "%s", miner_api );

#if defined(WIN32)
   webui_job_setup();
#endif
   atexit( webui_atexit );

   if ( pthread_create( &tid, NULL, webui_supervisor, NULL ) )
   {
      applog( LOG_ERR, "webui: cannot start the supervisor thread" );
      return;
   }
   pthread_detach( tid );

   applog( LOG_INFO, "webui: serving http://%s/ from %s", bind, child_path );
}

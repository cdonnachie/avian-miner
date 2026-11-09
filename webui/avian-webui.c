/*
 * avian-webui: a read-only web dashboard for avian-miner.
 *
 * Runs beside the miner rather than inside it. The miner's own API is a text
 * protocol on loopback; this polls it, keeps a little history in memory, and
 * serves a page that other machines on the LAN can open -- so a rig can be
 * watched from a desk without the hashing process also having to answer
 * requests from the network.
 *
 * It is a separate program and not part of the miner on purpose. The miner's
 * API is what holds seturl and quit behind --api-remote; this is the half
 * that gets exposed, and a parsing bug here costs a dashboard rather than a
 * hashrate. The two never share an address space.
 *
 * Read-only in three ways, each of them a property rather than a claim:
 *
 *   - the only commands it will send the miner are summary, threads and
 *     blocks, fixed in miner_commands; nothing out of an HTTP request ever
 *     reaches the miner socket, so there is no path to seturl or quit;
 *   - only GET and HEAD are answered, and everything else gets 405;
 *   - nothing is written to disk. History lives in memory and goes when the
 *     process does.
 *
 * There is no authentication, because adding one here would invite trusting
 * it. Bind it where you would be comfortable publishing the numbers, and
 * leave the miner's own API on 127.0.0.1.
 *
 * Licence: GNU GPL v3, as the rest of this program.
 */

#ifdef HAVE_CONFIG_H
#include "cpuminer-config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <jansson.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <sys/types.h>
#  include <sys/stat.h>
typedef SOCKET sock_t;
#  define SOCK_INVALID  INVALID_SOCKET
#  define sock_close    closesocket
#  define sock_errno()  WSAGetLastError()
#  define SHUT_WRITE    SD_SEND
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_INVALID  (-1)
#  define sock_close    close
#  define sock_errno()  errno
#  define SHUT_WRITE    SHUT_WR
#endif

#include "avian-webui-page.h"

#ifndef PACKAGE_VERSION
#  define PACKAGE_VERSION "unknown"
#endif

/* Everything this program is ever allowed to say to the miner. The
 * privileged commands are absent by design, and nothing adds to this list at
 * run time: it is the whole vocabulary, in the binary, and an HTTP request
 * cannot reach past it. */
static const char *const miner_commands[] = { "summary", "threads", "blocks" };

#define MAX_CARDS    256   /* a CPU run shows a row per thread, so not 16 */
#define MAX_BLOCKS    16   /* what the miner's own ring holds */
#define REQ_MAX      8192
/* Room for a 512 byte config path plus the sentence around it, so a long
 * path is reported whole rather than cut off mid-word. */
#define ERRMAX        800
#define REPLY_MAX  (256 * 1024)

/* ------------------------------------------------------------------ */
/* a growable buffer                                                    */
/* ------------------------------------------------------------------ */

struct buf {
   char   *p;
   size_t  len;
   size_t  cap;
   bool    bad;     /* an allocation failed; the buffer is no longer whole */
};

static void buf_free( struct buf *b )
{
   free( b->p );
   b->p = NULL;
   b->len = b->cap = 0;
   b->bad = false;
}

static bool buf_room( struct buf *b, size_t extra )
{
   size_t want;
   char *q;

   if ( b->bad ) return false;
   if ( b->len + extra + 1 <= b->cap ) return true;

   want = b->cap ? b->cap : 1024;
   while ( want < b->len + extra + 1 ) want *= 2;

   q = (char *) realloc( b->p, want );
   if ( !q ) { b->bad = true; return false; }
   b->p = q;
   b->cap = want;
   return true;
}

static void buf_add( struct buf *b, const char *s, size_t n )
{
   if ( !buf_room( b, n ) ) return;
   memcpy( b->p + b->len, s, n );
   b->len += n;
   b->p[ b->len ] = '\0';
}

static void buf_puts( struct buf *b, const char *s )
{
   buf_add( b, s, strlen( s ) );
}

static void buf_printf( struct buf *b, const char *fmt, ... )
{
   va_list ap;
   int n;

   va_start( ap, fmt );
   n = vsnprintf( NULL, 0, fmt, ap );
   va_end( ap );
   if ( n < 0 || !buf_room( b, (size_t) n ) ) return;

   va_start( ap, fmt );
   vsnprintf( b->p + b->len, b->cap - b->len, fmt, ap );
   va_end( ap );
   b->len += (size_t) n;
}

/* A JSON string, quotes included. Card names reach this from a GPU driver by
 * way of the miner's API, so they are not ours to trust: one unescaped quote
 * in one of them would turn the whole document into something the page
 * cannot parse, and the page would then show nothing with no reason given. */
static void buf_json_str( struct buf *b, const char *s )
{
   buf_puts( b, "\"" );
   for ( ; s && *s; s++ )
   {
      unsigned char c = (unsigned char) *s;

      switch ( c )
      {
         case '"':  buf_puts( b, "\\\"" ); break;
         case '\\': buf_puts( b, "\\\\" ); break;
         case '\n': buf_puts( b, "\\n" );  break;
         case '\r': buf_puts( b, "\\r" );  break;
         case '\t': buf_puts( b, "\\t" );  break;
         default:
            if ( c < 0x20 ) buf_printf( b, "\\u%04x", c );
            else            buf_add( b, (const char *) &c, 1 );
      }
   }
   buf_puts( b, "\"" );
}

/* A number, or null where the miner did not report one. Non-finite goes out
 * as null as well: JSON has no NaN, and a document the page cannot parse is
 * a worse answer than a missing field. */
static void buf_json_num( struct buf *b, double v, bool present )
{
   if ( !present || !isfinite( v ) ) buf_puts( b, "null" );
   else                              buf_printf( b, "%.15g", v );
}

/* ------------------------------------------------------------------ */
/* the miner's record format                                            */
/* ------------------------------------------------------------------ */

/*
 * One answer is "KEY=value;KEY=value|" repeated. Values can hold spaces --
 * card names do -- so only ; and | are structural.
 *
 * Returns the value of key within one record, or NULL. The record is the
 * text between two | and is not NUL terminated at its end, hence len.
 */
static const char *rec_field( const char *rec, size_t len, const char *key,
                              size_t *vlen )
{
   size_t klen = strlen( key );
   size_t i = 0;

   while ( i < len )
   {
      size_t start = i, eq, end;

      while ( i < len && rec[i] != ';' ) i++;
      end = i;
      if ( i < len ) i++;              /* step over the ; */

      for ( eq = start; eq < end && rec[eq] != '='; eq++ )
         ;
      if ( eq >= end ) continue;       /* no =, not a field */
      if ( eq - start != klen ) continue;
      if ( memcmp( rec + start, key, klen ) != 0 ) continue;

      *vlen = end - ( eq + 1 );
      return rec + eq + 1;
   }
   return NULL;
}

static bool field_str( const char *rec, size_t len, const char *key,
                       char *out, size_t n )
{
   size_t vlen;
   const char *v = rec_field( rec, len, key, &vlen );

   if ( !v ) { if ( n ) out[0] = '\0'; return false; }
   if ( vlen >= n ) vlen = n - 1;
   memcpy( out, v, vlen );
   out[ vlen ] = '\0';
   return true;
}

static bool field_num( const char *rec, size_t len, const char *key,
                       double *out )
{
   char tmp[64], *end;
   double v;

   if ( !field_str( rec, len, key, tmp, sizeof tmp ) || !tmp[0] ) return false;
   errno = 0;
   v = strtod( tmp, &end );
   if ( end == tmp || errno == ERANGE ) return false;
   *out = v;
   return true;
}

static bool field_long( const char *rec, size_t len, const char *key,
                        long *out )
{
   double v;

   if ( !field_num( rec, len, key, &v ) ) return false;
   *out = (long) v;
   return true;
}

/* The miner scales each thread's rate for display and names the prefix in the
 * key, so the key decides the multiplier rather than it being assumed. */
static bool field_rate( const char *rec, size_t len, double *out )
{
   static const struct { const char *key; double mul; } rates[] = {
      { "H/s",  1.0  }, { "kH/s", 1e3  }, { "MH/s", 1e6  },
      { "GH/s", 1e9  }, { "TH/s", 1e12 }, { "PH/s", 1e15 },
   };
   size_t i;

   for ( i = 0; i < sizeof rates / sizeof rates[0]; i++ )
      if ( field_num( rec, len, rates[i].key, out ) )
      {
         *out *= rates[i].mul;
         return true;
      }
   return false;
}

/* Walks the | separated records of one reply. */
struct recs { const char *p; const char *end; };

static bool recs_next( struct recs *r, const char **rec, size_t *len )
{
   while ( r->p < r->end )
   {
      const char *start = r->p;

      while ( r->p < r->end && *r->p != '|' ) r->p++;
      *rec = start;
      *len = (size_t) ( r->p - start );
      if ( r->p < r->end ) r->p++;      /* step over the | */

      if ( *len ) return true;          /* skip the empty tail */
   }
   return false;
}

/* ------------------------------------------------------------------ */
/* history                                                              */
/* ------------------------------------------------------------------ */

struct sample { double t; double hs; };

struct hist {
   struct sample *s;
   int cap, n, head;
};

static void hist_push( struct hist *h, int cap, double t, double hs )
{
   if ( !h->s )
   {
      h->s = (struct sample *) calloc( (size_t) cap, sizeof *h->s );
      if ( !h->s ) return;
      h->cap = cap;
   }
   h->s[ h->head ] = (struct sample) { t, hs };
   h->head = ( h->head + 1 ) % h->cap;
   if ( h->n < h->cap ) h->n++;
}

/* Oldest first, which is the order the chart draws them in. */
static void hist_json( struct buf *b, const struct hist *h )
{
   int i;

   buf_puts( b, "[" );
   for ( i = 0; i < h->n; i++ )
   {
      const struct sample *s = &h->s[ ( h->head - h->n + i + h->cap ) % h->cap ];

      if ( i ) buf_puts( b, "," );
      buf_printf( b, "{\"t\":%.1f,\"hs\":%.15g}", s->t, s->hs );
   }
   buf_puts( b, "]" );
}

/* ------------------------------------------------------------------ */
/* talking to the miner                                                 */
/* ------------------------------------------------------------------ */

struct miner {
   char host[128];
   int  port;
   int  timeout;        /* seconds */
};

static void sock_timeouts( sock_t s, int seconds )
{
#if defined(_WIN32)
   DWORD ms = (DWORD) seconds * 1000;
   setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &ms, sizeof ms );
   setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, (const char *) &ms, sizeof ms );
#else
   struct timeval tv = { seconds, 0 };
   setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv );
   setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv );
#endif
}

/* The code is passed in rather than read here, because the call that failed
 * is rarely the last one made: closesocket succeeding resets Winsock's
 * last error to zero, so a message built after the cleanup reads "The
 * operation completed successfully" over the top of a refused connection. */
static void sock_error_of( int err, char *out, size_t n )
{
#if defined(_WIN32)
   char *msg = NULL;

   FormatMessageA( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                 | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD) err,
                   MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
                   (LPSTR) &msg, 0, NULL );
   if ( msg )
   {
      size_t i = strlen( msg );
      while ( i && ( msg[i-1] == '\r' || msg[i-1] == '\n' || msg[i-1] == ' ' ) )
         msg[--i] = '\0';
      snprintf( out, n, "%s", msg );
      LocalFree( msg );
   }
   else
      snprintf( out, n, "winsock error %d", err );
#else
   snprintf( out, n, "%s", strerror( err ) );
#endif
}

static void sock_error( char *out, size_t n )
{
   sock_error_of( sock_errno(), out, n );
}

/*
 * One command, one connection, which is the miner's API and not a choice:
 * api.c closes the socket after a single reply, so there is nothing to hold
 * open between polls.
 */
static bool miner_ask( const struct miner *m, const char *cmd,
                       struct buf *out, char *err, size_t errlen )
{
   char port[16];
   struct addrinfo hints, *res = NULL, *ai;
   sock_t s = SOCK_INVALID;
   int failure = 0;
   char line[64];
   size_t i;
   bool ok = false;

   /* Not a safety check so much as a statement of the whole vocabulary: if
    * a caller is ever added that takes its command from somewhere else,
    * this is what stops it reaching the miner. */
   for ( i = 0; i < sizeof miner_commands / sizeof miner_commands[0]; i++ )
      if ( strcmp( cmd, miner_commands[i] ) == 0 ) break;
   if ( i == sizeof miner_commands / sizeof miner_commands[0] )
   {
      snprintf( err, errlen, "refusing to send %s to the miner", cmd );
      return false;
   }

   snprintf( port, sizeof port, "%d", m->port );
   memset( &hints, 0, sizeof hints );
   hints.ai_family   = AF_INET;         /* the miner's API binds AF_INET */
   hints.ai_socktype = SOCK_STREAM;

   if ( getaddrinfo( m->host, port, &hints, &res ) != 0 || !res )
   {
      snprintf( err, errlen, "cannot resolve %s", m->host );
      return false;
   }

   for ( ai = res; ai; ai = ai->ai_next )
   {
      s = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
      if ( s == SOCK_INVALID ) { failure = sock_errno(); continue; }
      sock_timeouts( s, m->timeout );
      if ( connect( s, ai->ai_addr, (int) ai->ai_addrlen ) == 0 ) break;
      /* Taken before the close, which would otherwise overwrite it. */
      failure = sock_errno();
      sock_close( s );
      s = SOCK_INVALID;
   }

   if ( s == SOCK_INVALID )
   {
      char why[160];

      sock_error_of( failure, why, sizeof why );
      snprintf( err, errlen, "no answer from the miner at %s:%d (%s)",
                m->host, m->port, why );
      freeaddrinfo( res );
      return false;
   }
   freeaddrinfo( res );

   snprintf( line, sizeof line, "%s\n", cmd );
   if ( send( s, line, (int) strlen( line ), 0 ) < 0 )
   {
      char why[160];

      sock_error( why, sizeof why );
      snprintf( err, errlen, "cannot ask the miner for %s (%s)", cmd, why );
      goto done;
   }
   shutdown( s, SHUT_WRITE );

   for (;;)
   {
      char chunk[4096];
      bool last = false;
      int n = recv( s, chunk, sizeof chunk, 0 );

      if ( n == 0 ) break;
      if ( n < 0 )
      {
         char why[160];

         sock_error( why, sizeof why );
         snprintf( err, errlen, "the miner stopped answering %s (%s)",
                   cmd, why );
         goto done;
      }
      /* The reply is NUL terminated; api.c sends strlen + 1 bytes. */
      for ( i = 0; i < (size_t) n; i++ )
         if ( chunk[i] == '\0' ) { n = (int) i; last = true; break; }

      buf_add( out, chunk, (size_t) n );
      if ( last ) break;
      if ( out->len > REPLY_MAX )
      {
         snprintf( err, errlen, "the miner's %s answer did not end", cmd );
         goto done;
      }
   }

   if ( out->bad ) { snprintf( err, errlen, "out of memory" ); goto done; }
   ok = true;

done:
   sock_close( s );
   return ok;
}

/* ------------------------------------------------------------------ */
/* the miner's config file, for --miner-config                          */
/* ------------------------------------------------------------------ */

/*
 * The address a miner given this api-bind will listen on.
 *
 * The three forms case 'b' in cpu-miner.c accepts: ip:port, an ip alone, or
 * a bare port. Mirrored rather than approximated -- a dashboard that reads a
 * different port out of the same string than the miner does is worse than
 * one that never looked, because it fails while appearing to know.
 */
static bool api_bind_address( const char *value, char *host, size_t hostlen,
                              int *port )
{
   const char *colon, *dot;
   char *end;
   long n;

   while ( *value == ' ' || *value == '\t' ) value++;
   if ( !*value ) return false;

   colon = strchr( value, ':' );       /* the first, as strstr finds it */
   dot   = strchr( value, '.' );

   if ( colon )
   {
      size_t len = (size_t) ( colon - value );

      if ( len >= hostlen ) return false;
      memcpy( host, value, len );
      host[ len ] = '\0';
      errno = 0;
      n = strtol( colon + 1, &end, 10 );
      if ( end == colon + 1 || errno == ERANGE ) return false;
   }
   else if ( dot )
   {
      snprintf( host, hostlen, "%s", value );
      n = 4048;                         /* an ip alone takes the default */
   }
   else
   {
      errno = 0;
      n = strtol( value, &end, 10 );    /* a bare port is loopback */
      if ( end == value || errno == ERANGE ) return false;
      snprintf( host, hostlen, "127.0.0.1" );
   }

   if ( n <= 0 || n > 65535 ) return false;
   *port = (int) n;

   /* A miner told to bind every interface is still reachable here over
    * loopback, and 0.0.0.0 is not an address you can connect to. */
   if ( !host[0] || !strcmp( host, "0.0.0.0" ) || !strcmp( host, "*" ) )
      snprintf( host, hostlen, "127.0.0.1" );
   return true;
}

struct minerconf {
   char   path[512];
   char   error[ERRMAX];
   char   host[128];
   int    port;
   bool   known;
   /* Re-read when the file moves rather than on every poll. HiveOS rewrites
    * it whenever a flight sheet is applied, so a rig whose API port moves
    * should not also need this restarted to follow it. */
   int64_t mtime;
   int64_t size;
};

static bool minerconf_resolve( struct minerconf *c )
{
   json_t *root, *bind;
   json_error_t jerr;
   const char *value;
#if defined(_WIN32)
   struct _stat64 st;
   if ( _stat64( c->path, &st ) != 0 )
#else
   struct stat st;
   if ( stat( c->path, &st ) != 0 )
#endif
   {
      c->known = false;
      snprintf( c->error, sizeof c->error, "cannot read %s (%s)",
                c->path, strerror( errno ) );
      return false;
   }

   /* Size as well as mtime: a config rewritten inside one mtime tick is
    * exactly what a script regenerating it looks like. */
   if ( c->known && (int64_t) st.st_mtime == c->mtime
                 && (int64_t) st.st_size  == c->size )
      return true;
   c->mtime = (int64_t) st.st_mtime;
   c->size  = (int64_t) st.st_size;

   root = json_load_file( c->path, 0, &jerr );
   if ( !root )
   {
      c->known = false;
      snprintf( c->error, sizeof c->error, "cannot parse %s (%s at line %d)",
                c->path, jerr.text, jerr.line );
      return false;
   }

   bind = json_object_get( root, "api-bind" );
   if ( !bind )
   {
      c->known = false;
      snprintf( c->error, sizeof c->error,
                "%s has no api-bind, so the miner is not serving its API "
                "at all", c->path );
      json_decref( root );
      return false;
   }

   /* A number is as valid as a string here: "api-bind": 4048 is what someone
    * writing the port on its own will type, and the miner's own parser takes
    * the text either way. */
   if ( json_is_integer( bind ) )
   {
      char tmp[32];

      snprintf( tmp, sizeof tmp, "%lld",
                (long long) json_integer_value( bind ) );
      value = tmp;
      if ( !api_bind_address( value, c->host, sizeof c->host, &c->port ) )
      {
         c->known = false;
         snprintf( c->error, sizeof c->error,
                   "cannot read an address out of the api-bind in %s: %.60s",
                   c->path, tmp );
         json_decref( root );
         return false;
      }
      c->known = true;
      c->error[0] = '\0';
      json_decref( root );
      return true;
   }

   value = json_string_value( bind );
   if ( !value
     || !api_bind_address( value, c->host, sizeof c->host, &c->port ) )
   {
      c->known = false;
      snprintf( c->error, sizeof c->error,
                "cannot read an address out of the api-bind in %s: %.60s",
                c->path, value ? value : "(not a string)" );
      json_decref( root );
      return false;
   }

   c->known = true;
   c->error[0] = '\0';
   json_decref( root );
   return true;
}

/* ------------------------------------------------------------------ */
/* shared state                                                         */
/* ------------------------------------------------------------------ */

/*
 * What a request serves. The poller builds the body once per poll and the
 * HTTP side only wraps it, so a request never waits on the miner: a page
 * load renders whatever the last poll produced, with its age, rather than
 * blocking on a socket that may be going nowhere.
 *
 * ok and error live outside the body so that a failed poll keeps the last
 * good figures and only changes the verdict on them -- the page dims the
 * frame rather than emptying it, and a dead rig's numbers never read as
 * current.
 */
struct shared {
   pthread_mutex_t lock;
   struct buf      body;           /* ",\"polled\":...,\"history\":[...]" */
   bool            ok;
   char            error[ERRMAX];
   char            miner[160];     /* host:port currently polled */
   double          interval;
};

static void shared_fail( struct shared *sh, const char *msg )
{
   pthread_mutex_lock( &sh->lock );
   sh->ok = false;
   snprintf( sh->error, sizeof sh->error, "%s", msg );
   pthread_mutex_unlock( &sh->lock );
}

/* ------------------------------------------------------------------ */
/* polling                                                              */
/* ------------------------------------------------------------------ */

struct card {
   long         thr;
   long         gpu, bus;
   bool         has_gpu, has_bus, has_rate;
   char         name[128];
   char         vendor[64];
   double       hashrate;
   long         solved;
   struct hist  hist;
};

struct poller {
   struct shared    *sh;
   struct miner      miner;
   struct minerconf *conf;
   double            interval;
   int               history;
   struct hist       total;
   struct card       cards[MAX_CARDS];
   int               ncards;
};

static double now_seconds( void )
{
#if defined(_WIN32)
   return (double) GetTickCount64() / 1000.0 + 1.0e9;  /* monotonic-ish */
#else
   struct timeval tv;

   gettimeofday( &tv, NULL );
   return (double) tv.tv_sec + (double) tv.tv_usec / 1e6;
#endif
}

static double wall_seconds( void )
{
   return (double) time( NULL );
}

static struct card *card_for( struct poller *p, long thr )
{
   int i;

   for ( i = 0; i < p->ncards; i++ )
      if ( p->cards[i].thr == thr ) return &p->cards[i];
   if ( p->ncards >= MAX_CARDS ) return NULL;

   memset( &p->cards[ p->ncards ], 0, sizeof p->cards[0] );
   p->cards[ p->ncards ].thr = thr;
   return &p->cards[ p->ncards++ ];
}

/* The threads record carries a card's identity, so a solve can name the card
 * even though the blocks record repeats it; and a block found on a card that
 * --devices has since dropped still gets whatever the miner last knew. */
static const struct card *card_lookup( const struct poller *p, long thr )
{
   int i;

   for ( i = 0; i < p->ncards; i++ )
      if ( p->cards[i].thr == thr ) return &p->cards[i];
   return NULL;
}

static void json_card_identity( struct buf *b, bool has_gpu, long gpu,
                                const char *name, const char *vendor,
                                bool has_bus, long bus )
{
   buf_puts( b, ",\"gpu\":" );
   buf_json_num( b, (double) gpu, has_gpu );
   buf_puts( b, ",\"name\":" );
   if ( name && *name ) buf_json_str( b, name ); else buf_puts( b, "null" );
   buf_puts( b, ",\"vendor\":" );
   if ( vendor && *vendor ) buf_json_str( b, vendor ); else buf_puts( b, "null" );
   buf_puts( b, ",\"bus\":" );
   buf_json_num( b, (double) bus, has_bus );
}

/*
 * Read the field, then emit it -- in that order, which is the whole point of
 * these existing. Written inline as
 *
 *     buf_json_num( b, (double) l, field_long( rec, len, "TS", &l ) )
 *
 * the same expression reads l and writes it with nothing sequencing the two,
 * so which value reached the buffer was down to the compiler's argument
 * order. It happened to be right here and would not have stayed that way.
 */
static void json_num_field( struct buf *b, const char *rec, size_t len,
                            const char *key )
{
   double v = 0.0;
   bool ok = field_num( rec, len, key, &v );

   buf_json_num( b, v, ok );
}

static void json_long_field( struct buf *b, const char *rec, size_t len,
                             const char *key )
{
   long v = 0;
   bool ok = field_long( rec, len, key, &v );

   buf_json_num( b, (double) v, ok );
}

static bool poll_once( struct poller *p, char *err, size_t errlen )
{
   struct buf summary = {0}, threads = {0}, blocks = {0}, body = {0};
   struct recs r;
   const char *rec;
   size_t len;
   bool gpu = false, ok = false;
   double total = 0.0, t;
   long l;
   char tmp[256];
   int i;

   if ( !miner_ask( &p->miner, "summary", &summary, err, errlen ) ) goto out;
   if ( !miner_ask( &p->miner, "threads", &threads, err, errlen ) ) goto out;
   if ( !miner_ask( &p->miner, "blocks",  &blocks,  err, errlen ) ) goto out;

   r.p = summary.p ? summary.p : "";
   r.end = r.p + summary.len;
   if ( !recs_next( &r, &rec, &len ) )
   {
      snprintf( err, errlen, "the miner answered summary with nothing" );
      goto out;
   }

   t = now_seconds();
   if ( field_num( rec, len, "HS", &total ) )
      hist_push( &p->total, p->history, wall_seconds(), total );

   /* ---- the whole document, bar ok and error ---- */

   buf_printf( &body, ",\"polled\":%.1f", wall_seconds() );

   buf_puts( &body, ",\"miner\":{" );
   field_str( rec, len, "NAME", tmp, sizeof tmp );
   buf_puts( &body, "\"name\":" ); buf_json_str( &body, tmp );
   field_str( rec, len, "VER", tmp, sizeof tmp );
   buf_puts( &body, ",\"version\":" ); buf_json_str( &body, tmp );
   field_str( rec, len, "API", tmp, sizeof tmp );
   buf_puts( &body, ",\"api\":" ); buf_json_str( &body, tmp );
   field_str( rec, len, "ALGO", tmp, sizeof tmp );
   buf_puts( &body, ",\"algo\":" ); buf_json_str( &body, tmp );
   field_str( rec, len, "URL", tmp, sizeof tmp );
   buf_puts( &body, ",\"url\":" ); buf_json_str( &body, tmp );
   buf_puts( &body, ",\"cpus\":" );
   buf_json_num( &body, field_long( rec, len, "CPUS", &l ) ? (double) l : 0, true );
   buf_puts( &body, ",\"uptime\":" );
   buf_json_num( &body, field_num( rec, len, "UPTIME", &t ) ? t : 0, true );
   buf_puts( &body, ",\"ts\":" );
   json_long_field( &body, rec, len, "TS" );
   buf_puts( &body, "}" );

   buf_puts( &body, ",\"totals\":{" );
   buf_puts( &body, "\"hashrate\":" ); buf_json_num( &body, total, true );
   buf_puts( &body, ",\"accepted\":" );
   buf_json_num( &body, field_long( rec, len, "ACC", &l ) ? (double) l : 0, true );
   buf_puts( &body, ",\"rejected\":" );
   buf_json_num( &body, field_long( rec, len, "REJ", &l ) ? (double) l : 0, true );
   /* STALE postdates this dashboard's first version, so absent means an
    * older miner rather than no stale shares. */
   buf_puts( &body, ",\"stale\":" );
   json_long_field( &body, rec, len, "STALE" );
   buf_puts( &body, ",\"solved\":" );
   buf_json_num( &body, field_long( rec, len, "SOL", &l ) ? (double) l : 0, true );
   buf_puts( &body, ",\"accmn\":" );
   json_num_field( &body, rec, len, "ACCMN" );
   buf_puts( &body, ",\"diff\":" );
   json_num_field( &body, rec, len, "DIFF" );
   buf_puts( &body, ",\"temp\":" );
   json_num_field( &body, rec, len, "TEMP" );
   buf_puts( &body, ",\"fan\":" );
   json_long_field( &body, rec, len, "FAN" );
   buf_puts( &body, ",\"freq\":" );
   json_long_field( &body, rec, len, "FREQ" );
   buf_puts( &body, "}" );

   /* ---- cards ---- */

   r.p = threads.p ? threads.p : "";
   r.end = r.p + threads.len;
   buf_puts( &body, ",\"cards\":[" );
   i = 0;
   while ( recs_next( &r, &rec, &len ) )
   {
      struct card *c;
      long thr = i;

      field_long( rec, len, "CPU", &thr );
      c = card_for( p, thr );
      if ( !c ) break;

      c->has_gpu = field_long( rec, len, "GPU", &c->gpu );
      c->has_bus = field_long( rec, len, "BUS", &c->bus );
      field_str( rec, len, "CARD", c->name, sizeof c->name );
      field_str( rec, len, "VENDOR", c->vendor, sizeof c->vendor );
      if ( !field_long( rec, len, "SOL", &c->solved ) ) c->solved = 0;
      c->has_rate = field_rate( rec, len, &c->hashrate );
      if ( c->has_gpu ) gpu = true;
      if ( c->has_rate )
         hist_push( &c->hist, p->history, wall_seconds(), c->hashrate );

      if ( i ) buf_puts( &body, "," );
      buf_printf( &body, "{\"thr\":%ld", thr );
      json_card_identity( &body, c->has_gpu, c->gpu, c->name, c->vendor,
                          c->has_bus, c->bus );
      buf_puts( &body, ",\"hashrate\":" );
      buf_json_num( &body, c->hashrate, c->has_rate );
      buf_printf( &body, ",\"solved\":%ld,\"history\":", c->solved );
      hist_json( &body, &c->hist );
      buf_puts( &body, "}" );
      i++;
   }
   buf_puts( &body, "]" );

   /* ---- blocks ---- */

   r.p = blocks.p ? blocks.p : "";
   r.end = r.p + blocks.len;
   buf_puts( &body, ",\"blocks\":[" );
   i = 0;
   while ( recs_next( &r, &rec, &len ) && i < MAX_BLOCKS )
   {
      long num = 0, ts = 0, height = 0, thr = 0, gpuno = 0, bus = 0;
      bool has_thr, has_gpu, has_bus;
      char name[128] = {0}, vendor[64] = {0};

      field_long( rec, len, "NUM", &num );
      field_long( rec, len, "TS", &ts );
      field_long( rec, len, "HEIGHT", &height );
      has_thr = field_long( rec, len, "CPU", &thr );
      has_gpu = field_long( rec, len, "GPU", &gpuno );
      has_bus = field_long( rec, len, "BUS", &bus );
      field_str( rec, len, "CARD", name, sizeof name );
      field_str( rec, len, "VENDOR", vendor, sizeof vendor );

      if ( has_thr )
      {
         const struct card *live = card_lookup( p, thr );

         if ( live )
         {
            if ( !has_gpu && live->has_gpu ) { gpuno = live->gpu; has_gpu = true; }
            if ( !has_bus && live->has_bus ) { bus = live->bus;   has_bus = true; }
            if ( !name[0] ) snprintf( name, sizeof name, "%s", live->name );
            if ( !vendor[0] ) snprintf( vendor, sizeof vendor, "%s", live->vendor );
         }
      }

      if ( i ) buf_puts( &body, "," );
      buf_printf( &body, "{\"num\":%ld,\"ts\":%ld,\"height\":%ld",
                  num, ts, height );
      buf_puts( &body, ",\"diff\":" );
      json_num_field( &body, rec, len, "DIFF" );
      buf_puts( &body, ",\"netdiff\":" );
      json_num_field( &body, rec, len, "NETDIFF" );
      /* A solve whose thread was lost to an overflowed stats queue carries
       * no card at all, and the page says so rather than guessing. */
      buf_puts( &body, ",\"thr\":" );
      buf_json_num( &body, (double) thr, has_thr );
      buf_puts( &body, ",\"gpu\":" );
      buf_json_num( &body, (double) gpuno, has_gpu );
      buf_puts( &body, ",\"card\":" );
      if ( name[0] ) buf_json_str( &body, name ); else buf_puts( &body, "null" );
      buf_puts( &body, ",\"bus\":" );
      buf_json_num( &body, (double) bus, has_bus );
      buf_puts( &body, "}" );
      i++;
   }
   buf_puts( &body, "]" );

   buf_printf( &body, ",\"gpu\":%s", gpu ? "true" : "false" );
   buf_puts( &body, ",\"history\":" );
   hist_json( &body, &p->total );

   if ( body.bad )
   {
      snprintf( err, errlen, "out of memory building the dashboard's answer" );
      goto out;
   }

   pthread_mutex_lock( &p->sh->lock );
   buf_free( &p->sh->body );
   p->sh->body = body;
   body = (struct buf) {0};             /* the shared copy owns it now */
   p->sh->ok = true;
   p->sh->error[0] = '\0';
   pthread_mutex_unlock( &p->sh->lock );
   ok = true;

out:
   buf_free( &summary );
   buf_free( &threads );
   buf_free( &blocks );
   buf_free( &body );
   return ok;
}

static void sleep_seconds( double s )
{
#if defined(_WIN32)
   Sleep( (DWORD) ( s * 1000.0 ) );
#else
   struct timespec ts;

   ts.tv_sec  = (time_t) s;
   ts.tv_nsec = (long) ( ( s - (double) ts.tv_sec ) * 1e9 );
   nanosleep( &ts, NULL );
#endif
}

/* Follow the miner's config file, when we were given one. A file that cannot
 * be read skips the poll rather than aiming it at a stale address, so the
 * page says what is wrong with the config instead of blaming a miner that
 * may be running perfectly well. */
static bool retarget( struct poller *p )
{
   if ( !p->conf ) return true;

   if ( !minerconf_resolve( p->conf ) )
   {
      shared_fail( p->sh, p->conf->error );
      return false;
   }

   if ( strcmp( p->conf->host, p->miner.host ) || p->conf->port != p->miner.port )
   {
      fprintf( stderr, "avian-webui: miner API is %s:%d, per %s\n",
               p->conf->host, p->conf->port, p->conf->path );
      fflush( stderr );
      snprintf( p->miner.host, sizeof p->miner.host, "%s", p->conf->host );
      p->miner.port = p->conf->port;

      pthread_mutex_lock( &p->sh->lock );
      snprintf( p->sh->miner, sizeof p->sh->miner, "%s:%d",
                p->miner.host, p->miner.port );
      pthread_mutex_unlock( &p->sh->lock );
   }
   return true;
}

static void *poll_thread( void *arg )
{
   struct poller *p = (struct poller *) arg;

   for (;;)
   {
      char err[ERRMAX];

      err[0] = '\0';
      if ( retarget( p ) && !poll_once( p, err, sizeof err ) )
         shared_fail( p->sh, err[0] ? err : "the miner did not answer" );
      sleep_seconds( p->interval );
   }
   return NULL;
}

/* ------------------------------------------------------------------ */
/* the HTTP side                                                        */
/* ------------------------------------------------------------------ */

static void send_all( sock_t s, const char *p, size_t n )
{
   while ( n )
   {
      int sent = send( s, p, (int) ( n > 1 << 20 ? 1 << 20 : n ), 0 );

      if ( sent <= 0 ) return;
      p += sent;
      n -= (size_t) sent;
   }
}

static void respond( sock_t s, int status, const char *reason,
                     const char *ctype, const char *extra,
                     const char *bodytext, size_t bodylen, bool want_body )
{
   char head[512];
   int n;

   n = snprintf( head, sizeof head,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lu\r\n"
                 "Connection: close\r\n"
                 "%s"
                 "\r\n",
                 status, reason, ctype, (unsigned long) bodylen,
                 extra ? extra : "" );
   if ( n < 0 ) return;
   send_all( s, head, (size_t) n );
   if ( want_body && bodylen ) send_all( s, bodytext, bodylen );
}

static void serve_state( sock_t s, struct shared *sh, bool want_body )
{
   struct buf out = {0};

   pthread_mutex_lock( &sh->lock );
   buf_printf( &out, "{\"ok\":%s,\"error\":", sh->ok ? "true" : "false" );
   if ( sh->ok || !sh->error[0] ) buf_puts( &out, "null" );
   else                           buf_json_str( &out, sh->error );
   if ( sh->body.p ) buf_add( &out, sh->body.p, sh->body.len );
   buf_printf( &out, ",\"now\":%.1f,\"interval\":%.15g,\"miner_api\":",
               (double) time( NULL ), sh->interval );
   buf_json_str( &out, sh->miner );
   buf_puts( &out, "}" );
   pthread_mutex_unlock( &sh->lock );

   if ( out.bad )
      respond( s, 500, "Internal Server Error", "text/plain; charset=utf-8",
               NULL, "out of memory\n", 14, want_body );
   else
      respond( s, 200, "OK", "application/json; charset=utf-8",
               "Cache-Control: no-store\r\n", out.p, out.len, want_body );
   buf_free( &out );
}

/*
 * One request, then the socket closes. Served one at a time, as the miner's
 * own API thread serves its callers: a dashboard polled every few seconds by
 * a handful of browsers has nothing to gain from concurrency, and the
 * timeouts below are what stop a client that stops talking from holding the
 * loop.
 */
static void serve( sock_t s, struct shared *sh )
{
   char req[REQ_MAX + 1];
   size_t len = 0;
   char *line_end, *method, *path, *sp;
   bool want_body;

   for (;;)
   {
      int n = recv( s, req + len, (int) ( REQ_MAX - len ), 0 );

      if ( n <= 0 ) return;
      len += (size_t) n;
      req[ len ] = '\0';
      if ( strstr( req, "\r\n\r\n" ) || strstr( req, "\n\n" ) ) break;
      if ( len >= REQ_MAX )
      {
         respond( s, 431, "Request Header Fields Too Large",
                  "text/plain; charset=utf-8", NULL, "too long\n", 9, true );
         return;
      }
   }

   line_end = strpbrk( req, "\r\n" );
   if ( !line_end ) return;
   *line_end = '\0';

   method = req;
   sp = strchr( method, ' ' );
   if ( !sp ) return;
   *sp = '\0';
   path = sp + 1;
   sp = strchr( path, ' ' );
   if ( sp ) *sp = '\0';

   if ( !strcmp( method, "GET" ) )       want_body = true;
   else if ( !strcmp( method, "HEAD" ) ) want_body = false;
   else
   {
      /* Allow is part of the contract rather than politeness: it is how a
       * client learns this server has no write side at all. */
      respond( s, 405, "Method Not Allowed", "text/plain; charset=utf-8",
               "Allow: GET, HEAD\r\n", "", 0, true );
      return;
   }

   sp = strchr( path, '?' );
   if ( sp ) *sp = '\0';
   len = strlen( path );
   while ( len > 1 && path[ len - 1 ] == '/' ) path[ --len ] = '\0';

   if ( !strcmp( path, "/" ) || !strcmp( path, "" ) )
      respond( s, 200, "OK", "text/html; charset=utf-8", NULL,
               avian_webui_page, sizeof avian_webui_page - 1, want_body );
   else if ( !strcmp( path, "/api/state" ) )
      serve_state( s, sh, want_body );
   else
      respond( s, 404, "Not Found", "text/plain; charset=utf-8", NULL,
               "not found\n", 10, want_body );
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static bool split_hostport( const char *text, char *host, size_t hostlen,
                            int *port, int defport )
{
   const char *colon = strrchr( text, ':' );
   char *end;
   long n;

   if ( !colon )
   {
      if ( strchr( text, '.' ) )        /* a bare host keeps the default */
      {
         snprintf( host, hostlen, "%s", text );
         *port = defport;
         return true;
      }
      errno = 0;
      n = strtol( text, &end, 10 );     /* a bare port is every interface */
      if ( end == text || *end || n <= 0 || n > 65535 ) return false;
      snprintf( host, hostlen, "0.0.0.0" );
      *port = (int) n;
      return true;
   }

   if ( (size_t) ( colon - text ) >= hostlen ) return false;
   memcpy( host, text, (size_t) ( colon - text ) );
   host[ colon - text ] = '\0';
   if ( !host[0] ) snprintf( host, hostlen, "0.0.0.0" );

   errno = 0;
   n = strtol( colon + 1, &end, 10 );
   if ( end == colon + 1 || *end || n <= 0 || n > 65535 ) return false;
   *port = (int) n;
   return true;
}

static void usage( const char *me, FILE *out )
{
   fprintf( out,
"usage: %s [options]\n"
"\n"
"A read-only web dashboard for avian-miner. Polls the miner's own API and\n"
"serves a page other machines can open. It never sends the miner anything\n"
"but summary, threads and blocks, and answers only GET and HEAD.\n"
"\n"
"  --bind ADDR[:PORT]    where to serve the dashboard. The default listens\n"
"                        on every interface so another machine can open it;\n"
"                        use 127.0.0.1:4049 to keep it to this one.\n"
"                        (default: 0.0.0.0:4049)\n"
"  --miner ADDR[:PORT]   the miner's own API, which it has to be told to\n"
"                        open with --api-bind (default: 127.0.0.1:4048)\n"
"  --miner-config PATH   the miner's own config file, the one passed to its\n"
"                        -c. The api-bind in it decides the address to poll,\n"
"                        so the two cannot disagree about the port, and it\n"
"                        is re-read when the file changes, so a rig whose\n"
"                        API moves does not need this restarted. Takes\n"
"                        precedence over --miner. On HiveOS the file is\n"
"                        /hive/miners/custom/avian-miner/avian-miner.conf\n"
"  --interval SECONDS    seconds between polls (default: 5)\n"
"  --history SAMPLES     hashrate samples held in memory, per card and in\n"
"                        total; at the default interval 720 is an hour\n"
"                        (default: 720)\n"
"  --timeout SECONDS     miner API socket timeout (default: 5)\n"
"  --verbose             log every request\n"
"  --version             print the version and exit\n"
"  -h, --help            this\n",
            me );
}

int main( int argc, char **argv )
{
   static struct option opts[] = {
      { "bind",         1, NULL, 'b' },
      { "miner",        1, NULL, 'm' },
      { "miner-config", 1, NULL, 'c' },
      { "interval",     1, NULL, 'i' },
      { "history",      1, NULL, 'n' },
      { "timeout",      1, NULL, 't' },
      { "verbose",      0, NULL, 'v' },
      { "version",      0, NULL, 'V' },
      { "help",         0, NULL, 'h' },
      { NULL, 0, NULL, 0 }
   };
   const char *bind_arg = "0.0.0.0:4049";
   const char *miner_arg = "127.0.0.1:4048";
   const char *conf_arg = NULL;
   double interval = 5.0, timeout = 5.0;
   int history = 720, verbose = 0, opt;
   char bind_host[128];
   int bind_port;
   struct shared sh;
   struct minerconf conf;
   struct poller poller;
   pthread_t tid;
   sock_t listener;
   struct sockaddr_in addr;

   while ( ( opt = getopt_long( argc, argv, "h", opts, NULL ) ) != -1 )
   {
      switch ( opt )
      {
         case 'b': bind_arg  = optarg; break;
         case 'm': miner_arg = optarg; break;
         case 'c': conf_arg  = optarg; break;
         case 'i': interval  = atof( optarg ); break;
         case 'n': history   = atoi( optarg ); break;
         case 't': timeout   = atof( optarg ); break;
         case 'v': verbose   = 1; break;
         case 'V': printf( "avian-webui %s\n", PACKAGE_VERSION ); return 0;
         case 'h': usage( argv[0], stdout ); return 0;
         default:  usage( argv[0], stderr ); return 2;
      }
   }

   if ( interval <= 0 ) { fprintf( stderr, "--interval must be positive\n" ); return 2; }
   if ( history  <  2 ) { fprintf( stderr, "--history must be at least 2\n" ); return 2; }
   if ( timeout  <= 0 ) { fprintf( stderr, "--timeout must be positive\n" ); return 2; }

   memset( &sh, 0, sizeof sh );
   memset( &conf, 0, sizeof conf );
   memset( &poller, 0, sizeof poller );

   if ( !split_hostport( bind_arg, bind_host, sizeof bind_host, &bind_port, 4049 ) )
   {
      fprintf( stderr, "cannot read an address out of --bind %s\n", bind_arg );
      return 2;
   }
   if ( !split_hostport( miner_arg, poller.miner.host, sizeof poller.miner.host,
                         &poller.miner.port, 4048 ) )
   {
      fprintf( stderr, "cannot read an address out of --miner %s\n", miner_arg );
      return 2;
   }

#if defined(_WIN32)
   {
      WSADATA wsa;

      /* Nothing else in this program initialises Winsock, unlike the miner,
       * which gets it from libcurl. */
      if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
      {
         fprintf( stderr, "cannot start Winsock\n" );
         return 1;
      }
   }
#endif

   poller.miner.timeout = (int) ( timeout < 1.0 ? 1.0 : timeout );
   poller.interval = interval;
   poller.history  = history;
   poller.sh       = &sh;

   if ( conf_arg )
   {
      snprintf( conf.path, sizeof conf.path, "%s", conf_arg );
      poller.conf = &conf;
   }

   pthread_mutex_init( &sh.lock, NULL );
   sh.interval = interval;
   sh.ok = false;
   snprintf( sh.error, sizeof sh.error, "waiting for the first poll" );
   snprintf( sh.miner, sizeof sh.miner, "%s:%d",
             poller.miner.host, poller.miner.port );

   listener = socket( AF_INET, SOCK_STREAM, 0 );
   if ( listener == SOCK_INVALID )
   {
      fprintf( stderr, "cannot make a socket\n" );
      return 1;
   }
#if !defined(_WIN32)
   /* As api.c explains: on Linux this takes the port back from a closed
    * socket still in TIME_WAIT but still fails if another program holds it,
    * which is what we want. On Windows the same option lets a second program
    * steal a port in use, so it is left alone there. */
   {
      int one = 1;

      setsockopt( listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one );
   }
#endif

   memset( &addr, 0, sizeof addr );
   addr.sin_family = AF_INET;
   addr.sin_port   = htons( (unsigned short) bind_port );
   addr.sin_addr.s_addr = inet_addr( bind_host );
   if ( addr.sin_addr.s_addr == INADDR_NONE && strcmp( bind_host, "255.255.255.255" ) )
   {
      fprintf( stderr, "cannot bind to %s: not an address\n", bind_host );
      return 1;
   }

   if ( bind( listener, (struct sockaddr *) &addr, sizeof addr ) != 0 )
   {
      char why[160];

      sock_error( why, sizeof why );
      fprintf( stderr, "cannot bind %s:%d (%s)\n", bind_host, bind_port, why );
      return 1;
   }
   if ( listen( listener, 16 ) != 0 )
   {
      char why[160];

      sock_error( why, sizeof why );
      fprintf( stderr, "cannot listen on %s:%d (%s)\n", bind_host, bind_port, why );
      return 1;
   }

   if ( pthread_create( &tid, NULL, poll_thread, &poller ) != 0 )
   {
      fprintf( stderr, "cannot start the polling thread\n" );
      return 1;
   }
   pthread_detach( tid );

   fprintf( stderr, "avian-webui %s: reading %s, serving http://%s:%d/ "
                    "(read only)\n", PACKAGE_VERSION,
            conf_arg ? conf_arg : sh.miner,
            strcmp( bind_host, "0.0.0.0" ) ? bind_host : "<this host>",
            bind_port );
   if ( !strcmp( bind_host, "0.0.0.0" ) )
      fprintf( stderr, "avian-webui: reachable from the network and "
                       "unauthenticated; keep it off untrusted ones\n" );
   fflush( stderr );

   for (;;)
   {
      struct sockaddr_in from;
#if defined(_WIN32)
      int fromlen = sizeof from;
#else
      socklen_t fromlen = sizeof from;
#endif
      sock_t c = accept( listener, (struct sockaddr *) &from, &fromlen );

      if ( c == SOCK_INVALID ) continue;
      /* Shorter than the miner timeout and for a different reason: this one
       * bounds how long a client that stops talking holds the accept loop. */
      sock_timeouts( c, 5 );
      if ( verbose )
      {
         fprintf( stderr, "avian-webui: request from %s\n",
                  inet_ntoa( from.sin_addr ) );
         fflush( stderr );
      }
      serve( c, &sh );
      sock_close( c );
   }
}

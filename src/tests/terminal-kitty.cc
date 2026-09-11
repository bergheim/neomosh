/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

/* Unit tests for the server-side Kitty graphics protocol emulator: APC
   parsing, chunk assembly, replies and q gating, store caps, scroll-off,
   ED, and partial-admission-free Complete equality. */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "completeterminal.h"
#include "hostinput.pb.h"
#include "kittygraphics.h"

using namespace Terminal;

static int failures = 0;

static void check( bool ok, const std::string& what )
{
  if ( !ok ) {
    fprintf( stderr, "FAILED: %s\n", what.c_str() );
    failures++;
  }
}

static void check_eq( const std::string& actual, const std::string& expected, const std::string& what )
{
  if ( actual != expected ) {
    fprintf( stderr,
             "FAILED: %s (expected %zu bytes [%s], got %zu bytes [%s])\n",
             what.c_str(),
             expected.size(),
             expected.c_str(),
             actual.size(),
             actual.c_str() );
    failures++;
  }
}

static std::string base64_encode( const std::string& data )
{
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while ( i + 3 <= data.size() ) {
    unsigned int n
      = ( (unsigned char)data[i] << 16 ) | ( (unsigned char)data[i + 1] << 8 ) | (unsigned char)data[i + 2];
    out.push_back( table[( n >> 18 ) & 0x3F] );
    out.push_back( table[( n >> 12 ) & 0x3F] );
    out.push_back( table[( n >> 6 ) & 0x3F] );
    out.push_back( table[n & 0x3F] );
    i += 3;
  }
  size_t rem = data.size() - i;
  if ( rem == 1 ) {
    unsigned int n = (unsigned char)data[i] << 16;
    out.push_back( table[( n >> 18 ) & 0x3F] );
    out.push_back( table[( n >> 12 ) & 0x3F] );
    out.append( "==" );
  } else if ( rem == 2 ) {
    unsigned int n = ( (unsigned char)data[i] << 16 ) | ( (unsigned char)data[i + 1] << 8 );
    out.push_back( table[( n >> 18 ) & 0x3F] );
    out.push_back( table[( n >> 12 ) & 0x3F] );
    out.push_back( table[( n >> 6 ) & 0x3F] );
    out.push_back( '=' );
  }
  return out;
}

static std::string kitty_apc( const std::string& controls, const std::string& payload_b64 = "" )
{
  std::string s = "\033_G";
  s += controls;
  if ( !payload_b64.empty() ) {
    s += ";";
    s += payload_b64;
  }
  s += "\033\\";
  return s;
}

/* A minimal PNG: real 8-byte signature and IHDR chunk with the requested
   width/height at bytes 16..23; everything after that is garbage, since the
   server never decodes pixels. */
static std::string minimal_png( uint32_t width, uint32_t height, size_t extra_garbage = 8 )
{
  std::string p;
  const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  p.append( (const char*)sig, 8 );
  const unsigned char len[4] = { 0, 0, 0, 13 };
  p.append( (const char*)len, 4 );
  p.append( "IHDR", 4 );
  const unsigned char w[4] = { (unsigned char)( width >> 24 ),
                               (unsigned char)( width >> 16 ),
                               (unsigned char)( width >> 8 ),
                               (unsigned char)width };
  const unsigned char h[4] = { (unsigned char)( height >> 24 ),
                               (unsigned char)( height >> 16 ),
                               (unsigned char)( height >> 8 ),
                               (unsigned char)height };
  p.append( (const char*)w, 4 );
  p.append( (const char*)h, 4 );
  p.append( extra_garbage, 'x' );
  return p;
}

/* Send payload as m=1 chunks (6000 raw bytes each, comfortably under the
   per-APC dispatcher cap once base64-encoded) followed by a
   finalizing m=0 chunk. Returns the final reply. */
static std::string kitty_chunked_transmit( Complete& term,
                                           const std::string& first_controls,
                                           const std::string& payload )
{
  const size_t chunk_bytes = 6000;
  size_t sent = 0;
  std::string reply;
  bool first = true;
  while ( sent < payload.size() ) {
    size_t take = std::min( chunk_bytes, payload.size() - sent );
    bool last = ( sent + take == payload.size() );
    std::string controls = first ? ( first_controls + ",m=1" ) : ( last ? "m=0" : "m=1" );
    reply = term.act( kitty_apc( controls, base64_encode( payload.substr( sent, take ) ) ) );
    sent += take;
    first = false;
  }
  return reply;
}

/* True if a hostbytes-carrying diff (already-parsed HostMessage) contains
   the given Kitty APC prefix anywhere in its escape string. */
static bool diff_hostbytes_contains( const HostBuffers::HostMessage& msg, const std::string& needle )
{
  for ( int i = 0; i < msg.instruction_size(); i++ ) {
    if ( msg.instruction( i ).HasExtension( HostBuffers::hostbytes )
         && msg.instruction( i ).GetExtension( HostBuffers::hostbytes ).hoststring().find( needle )
              != std::string::npos ) {
      return true;
    }
  }
  return false;
}

int main( void )
{
  /* a=q with t=d replies OK and stores nothing. */
  {
    Complete term( 80, 24 );
    std::string reply
      = term.act( kitty_apc( "a=q,i=1,f=32,s=1,v=1,t=d", base64_encode( std::string( 4, '\0' ) ) ) );
    check_eq( reply, "\033_Gi=1;OK\033\\", "a=q with t=d replies OK" );
    check( term.get_fb().image_store_bytes() == 0, "a=q stores nothing" );
  }

  /* a=q with t=f replies an error. */
  {
    Complete term( 80, 24 );
    std::string reply = term.act( kitty_apc( "a=q,i=2,t=f" ) );
    check_eq( reply, "\033_Gi=2;EBADF:unsupported transmission medium\033\\", "a=q with t=f replies an error" );
  }

  /* a=T with f=100 and a real minimal PNG stores width/height from IHDR and
     places it at the cursor, moving the cursor. */
  {
    Complete term( 80, 24 );
    std::string png = minimal_png( 40, 20 );
    std::string reply = term.act( kitty_apc( "a=T,i=3,f=100", base64_encode( png ) ) );
    check_eq( reply, "\033_Gi=3;OK\033\\", "a=T with f=100 replies OK" );

    uint32_t internal = term.get_fb().image_store_resolve( 3 );
    check( internal != 0, "f=100 image is resolvable by app id" );
    auto image = term.get_fb().image_store_get( internal );
    check( image && image->format == 100 && image->width == 40 && image->height == 20,
           "f=100 image stores width/height from IHDR" );

    check( term.get_fb().get_row( 0 )->placements.size() == 1, "f=100 placed a placement at the cursor row" );
    check( term.get_fb().ds.get_cursor_row() == 0 && term.get_fb().ds.get_cursor_col() == 1,
           "f=100 placement moved the cursor" );
  }

  /* a=T with f=24, s=2, v=2 and 12 payload bytes. */
  {
    Complete term( 80, 24 );
    std::string pixels( 12, '\x7F' );
    std::string reply = term.act( kitty_apc( "a=T,i=4,f=24,s=2,v=2", base64_encode( pixels ) ) );
    check_eq( reply, "\033_Gi=4;OK\033\\", "a=T with f=24 replies OK" );
    uint32_t internal = term.get_fb().image_store_resolve( 4 );
    auto image = term.get_fb().image_store_get( internal );
    check( image && image->format == 24 && image->width == 2 && image->height == 2,
           "f=24 image stores width/height from s/v" );
  }

  /* Two m=1 chunks followed by m=0 assemble into one image; the reply comes
     once, at the end. */
  {
    Complete term( 80, 24 );
    std::string data = "0123456789AB"; /* 12 bytes, split 6/6 */
    std::string r1 = term.act( kitty_apc( "a=T,i=5,f=24,s=2,v=2,m=1", base64_encode( data.substr( 0, 6 ) ) ) );
    check_eq( r1, "", "first m=1 chunk produces no reply" );
    std::string r2 = term.act( kitty_apc( "m=0", base64_encode( data.substr( 6, 6 ) ) ) );
    check_eq( r2, "\033_Gi=5;OK\033\\", "chunk finalization replies exactly once" );

    uint32_t internal = term.get_fb().image_store_resolve( 5 );
    auto image = term.get_fb().image_store_get( internal );
    check( image && *image->blob == data, "chunked payload bytes were assembled in order" );
  }

  /* c= and r= extent. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Resize( 80, 24, 800, 480 ) ); /* 10x20 px cells */
    std::string png = minimal_png( 100, 100 );
    term.act( kitty_apc( "a=T,i=6,f=100,c=3,r=2", base64_encode( png ) ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "explicit c/r placement created" );
    const auto& placement = term.get_fb().get_row( 0 )->placements[0];
    check( placement->columns == 3 && placement->rows == 2, "explicit c and r used as the extent" );
  }

  /* No i and no I gives no reply, but the placement still happens. */
  {
    Complete term( 80, 24 );
    std::string reply = term.act( kitty_apc( "a=T,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check_eq( reply, "", "no i and no I gives no reply" );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "placement still happens without i" );
  }

  /* q=1 suppresses OK only; q=2 suppresses everything. */
  {
    Complete term( 80, 24 );
    std::string r1 = term.act( kitty_apc( "a=T,i=8,f=24,s=1,v=1,q=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check_eq( r1, "", "q=1 suppresses the OK reply" );

    std::string r2 = term.act( kitty_apc( "a=q,i=9,t=f,q=1" ) );
    check_eq( r2, "\033_Gi=9;EBADF:unsupported transmission medium\033\\", "q=1 does not suppress an error reply" );

    std::string r3 = term.act( kitty_apc( "a=T,i=10,f=24,s=1,v=1,q=2", base64_encode( std::string( 3, '\0' ) ) ) );
    check_eq( r3, "", "q=2 suppresses the OK reply" );

    std::string r4 = term.act( kitty_apc( "a=q,i=11,t=f,q=2" ) );
    check_eq( r4, "", "q=2 suppresses the error reply too" );
  }

  /* ED clears the placement. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=12,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "placement exists before ED" );
    term.act( "\033[2J" );
    check( term.get_fb().get_row( 0 )->placements.size() == 0, "ED clears the placement" );
  }

  /* A placement scrolls off when enough line feeds push its row past the top. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=13,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "placement created at row 0" );
    term.act( "\033[24;1H" );
    for ( int i = 0; i < 24; i++ ) {
      term.act( "\n" );
    }
    check( term.get_fb().get_row( 0 )->placements.size() == 0, "placement scrolled off the top" );
  }

  /* An image taller than the remaining rows scrolls the screen; the anchor
     row moves up. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Resize( 80, 24, 800, 480 ) ); /* 10x20 px cells */
    term.act( "\033[20;1H" );                       /* row index 19; 5 rows remain below */
    std::string png = minimal_png( 100, 100 );
    term.act( kitty_apc( "a=T,i=14,f=100,r=10", base64_encode( png ) ) );
    check( term.get_fb().ds.get_cursor_row() == 23, "cursor ends at the bottom row after scroll-placement" );
    uint32_t internal = term.get_fb().image_store_resolve( 14 );
    bool found = false;
    for ( const auto& p : term.get_fb().get_row( 14 )->placements ) {
      if ( p->internal_image_id == internal ) {
        found = true;
      }
    }
    check( found, "image taller than the remaining rows anchors at height - rows" );
  }

  /* a=d,d=a removes placements and keeps data; d=A frees data too. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=20,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "placement exists before delete" );

    term.act( kitty_apc( "a=d,d=a" ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 0, "d=a removed the placement" );

    std::string reply = term.act( kitty_apc( "a=p,i=20" ) );
    check_eq( reply, "\033_Gi=20;OK\033\\", "d=a kept the image data; a=p can still place it" );

    term.act( kitty_apc( "a=d,d=A" ) );
    std::string reply2 = term.act( kitty_apc( "a=p,i=20" ) );
    check_eq( reply2, "\033_Gi=20;ENOENT\033\\", "d=A freed the image data too" );
  }

  /* Re-sending an existing i replaces the image and drops old placements. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=30,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "first transmit placed at row 0" );
    uint32_t old_internal = term.get_fb().image_store_resolve( 30 );

    term.act( kitty_apc( "a=T,i=30,f=24,s=1,v=1", base64_encode( std::string( 3, '\x01' ) ) ) );
    uint32_t new_internal = term.get_fb().image_store_resolve( 30 );
    check( new_internal != old_internal, "re-transmit allocates a new internal id" );
    check( term.get_fb().get_row( 0 )->placements.size() == 1
             && term.get_fb().get_row( 0 )->placements[0]->internal_image_id == new_internal,
           "re-transmit replaces the placement and drops the old one" );
  }

  /* The 8 MiB cap replies EFBIG. Sent chunked, since a single APC command is
     capped at 8192 bytes by the dispatcher. */
  {
    Complete term( 80, 24 );
    const size_t chunk_bytes = 6000; /* multiple of 3, decodes cleanly */
    std::string chunk_raw( chunk_bytes, 'z' );
    std::string chunk_b64 = base64_encode( chunk_raw );

    size_t total = 0;
    const size_t target = Kitty::IMAGE_MAX_BYTES + 1;
    bool first = true;
    while ( total < target ) {
      std::string controls = first ? "a=T,i=40,f=24,s=1,v=1,m=1" : "m=1";
      term.act( kitty_apc( controls, chunk_b64 ) );
      total += chunk_bytes;
      first = false;
    }
    std::string final_reply = term.act( kitty_apc( "m=0", base64_encode( std::string( 3, 'z' ) ) ) );
    check_eq( final_reply, "\033_Gi=40;EFBIG\033\\", "an image over IMAGE_MAX_BYTES replies EFBIG" );
  }

  /* Regression: kitten icat sends a whole image as ONE unchunked APC. The
     dispatcher used to cap every APC at 8192 bytes and discard larger ones
     silently, which threw away every real image over about 6 KB while tiny
     icons still worked. A single 30000-byte payload (40000 base64 chars)
     must now be stored and placed. */
  {
    Complete term( 80, 24 );
    std::string reply
      = term.act( kitty_apc( "a=T,i=50,f=24,s=100,v=100", base64_encode( std::string( 30000, 'p' ) ) ) );
    check_eq( reply, "\033_Gi=50;OK\033\\", "a single unchunked APC well over 8192 bytes replies OK" );
    check( term.get_fb().image_store_resolve( 50 ) != 0, "the large unchunked image is stored" );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "the large unchunked image is placed" );
  }

  /* The APC cap still exists, sized above the largest acceptable image; an
     APC over it is discarded silently, so a hostile app cannot grow the
     dispatcher buffer without bound. */
  {
    Complete term( 80, 24 );
    const size_t over_cap = Kitty::IMAGE_MAX_BYTES / 3 * 4 + 65536 + 1;
    std::string reply = term.act( kitty_apc( "a=T,i=51,f=24,s=1,v=1", std::string( over_cap, 'A' ) ) );
    check_eq( reply, "", "an APC command over the dispatcher cap is discarded silently" );
    check( term.get_fb().image_store_resolve( 51 ) == 0, "discarded APC command stored nothing" );
  }

  /* A copy of a Complete equals the original and differs after a placement. */
  {
    Complete term( 80, 24 );
    Complete term2( term );
    check( term == term2, "a fresh copy equals the original" );
    term2.act( kitty_apc( "a=T,i=60,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check( !( term == term2 ), "a copy differs from the original after a placement" );
  }

  /* Fix: the chunk accumulator is capped while accumulating, not only at
     finalize, so a remote app cannot grow it without bound by never sending
     m=0. Driven directly through Kitty::handle_apc (bypassing the
     per-APC-command dispatcher cap, which is a different, unrelated limit)
     so each "chunk" can carry far more than one escape sequence could. */
  {
    Framebuffer fb( 80, 24 );
    Kitty::ChunkState chunk;
    std::string one_mib_b64 = base64_encode( std::string( 1024 * 1024, 'z' ) );

    std::string reply;
    const int n_chunks = static_cast<int>( Kitty::IMAGE_MAX_BYTES / ( 1024 * 1024 ) ) + 1; /* one over the cap */
    for ( int i = 0; i < n_chunks; i++ ) {
      bool last = ( i == n_chunks - 1 );
      std::string controls = ( i == 0 ) ? "a=T,i=71,f=24,s=1,v=1,m=1" : ( last ? "m=0" : "m=1" );
      reply = Kitty::handle_apc( "G" + controls + ";" + one_mib_b64, &fb, &chunk );
      if ( !last ) {
        check_eq( reply, "", "unbounded chunk growth: no reply before the final chunk" );
      }
    }
    check_eq( reply, "\033_Gi=71;EFBIG\033\\", "unbounded chunk growth: exactly one EFBIG reply at the end" );
    check( fb.image_store_bytes() == 0, "unbounded chunk growth: nothing stored" );
  }

  /* Fix: a continuation chunk that repeats a=/i= (as real clients do), not
     just bare m=/q=, still continues the transmission instead of aborting
     it. */
  {
    Complete term( 80, 24 );
    std::string data = "abcdefghijkl"; /* 12 bytes, split 6/6 */
    std::string r1 = term.act( kitty_apc( "a=T,i=7,f=24,s=2,v=2,m=1", base64_encode( data.substr( 0, 6 ) ) ) );
    check_eq( r1, "", "repeated-key continuation: first chunk produces no reply" );
    std::string r2 = term.act( kitty_apc( "a=T,i=7,m=0", base64_encode( data.substr( 6, 6 ) ) ) );
    check_eq( r2, "\033_Gi=7;OK\033\\", "repeated-key continuation: finalize replies exactly once" );

    uint32_t internal = term.get_fb().image_store_resolve( 7 );
    auto image = term.get_fb().image_store_get( internal );
    check( image && *image->blob == data, "repeated-key continuation: bytes assembled in order" );
  }

  /* Fix: a genuinely different command interrupts an in-progress chunked
     transmission, replying EINVAL for the aborted one, then executes
     normally, with both replies concatenated. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=7,f=24,s=2,v=2,m=1", base64_encode( std::string( 6, 'x' ) ) ) );
    std::string reply = term.act( kitty_apc( "a=q,i=9,f=32,s=1,v=1" ) );
    check_eq( reply,
              "\033_Gi=7;EINVAL:chunked transmission interrupted\033\\"
              "\033_Gi=9;OK\033\\",
              "an interrupting command gets EINVAL for the old id then its own reply" );
  }

  /* Fix: ENOSPC is sent when eviction cannot free enough room, and deleting
     a placement (freeing it for later eviction) lets a retry succeed. */
  {
    Complete term( 80, 24 );
    Framebuffer::set_kitty_store_cap_for_tests( 5000 );

    /* f=32, s=1, v=500 needs exactly 1*500*4 = 2000 bytes, matching the
       exact-payload-size validation. */
    std::string img1( 2000, 'a' );
    std::string img2( 2000, 'b' );
    std::string img3( 2000, 'c' );

    check_eq( term.act( kitty_apc( "a=T,i=80,f=32,s=1,v=500", base64_encode( img1 ) ) ),
              "\033_Gi=80;OK\033\\",
              "ENOSPC: first image stored" );
    check_eq( term.act( kitty_apc( "a=T,i=81,f=32,s=1,v=500", base64_encode( img2 ) ) ),
              "\033_Gi=81;OK\033\\",
              "ENOSPC: second image stored, cap now full" );
    check_eq( term.act( kitty_apc( "a=T,i=82,f=32,s=1,v=500", base64_encode( img3 ) ) ),
              "\033_Gi=82;ENOSPC\033\\",
              "ENOSPC: third image over the cap with nothing evictable" );
    check( term.get_fb().image_store_resolve( 82 ) == 0, "ENOSPC: third image was not stored" );

    term.act( kitty_apc( "a=d,d=i,i=80" ) );
    check_eq( term.act( kitty_apc( "a=T,i=82,f=32,s=1,v=500", base64_encode( img3 ) ) ),
              "\033_Gi=82;OK\033\\",
              "ENOSPC: deleting the first placement frees it up for eviction, retry succeeds" );

    Framebuffer::set_kitty_store_cap_for_tests( 64 * 1024 * 1024 ); /* restore the default for later tests */
  }

  /* base64_decode's accumulator must not misbehave (nor, under UBSan, trip
     signed-shift UB) on input well beyond a few bytes. */
  {
    Complete term( 80, 24 );
    std::string original( 3000, '\0' );
    for ( size_t i = 0; i < original.size(); i++ ) {
      original[i] = static_cast<char>( i % 251 );
    }
    check_eq( term.act( kitty_apc( "a=t,i=95,f=32,s=1,v=750", base64_encode( original ) ) ),
              "\033_Gi=95;OK\033\\",
              "3000-byte payload round trip stores OK" );
    uint32_t internal = term.get_fb().image_store_resolve( 95 );
    auto image = term.get_fb().image_store_get( internal );
    check( image && *image->blob == original, "3000-byte payload round trip decodes correctly" );
  }

  /* The anchor column (cursor column at placement time) is stored on the
     placement. */
  {
    Complete term( 80, 24 );
    term.act( "\033[1;6H" ); /* row 0, column 5 */
    term.act( kitty_apc( "a=T,i=110,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "placement created at column 5" );
    check( term.get_fb().get_row( 0 )->placements[0]->column == 5, "anchor column recorded as 5" );
  }

  /* An absurd c is rejected outright (no clamped-but-still-huge placement,
     no undefined behaviour computing orig_col + cols). */
  {
    Complete term( 80, 24 );
    std::string reply
      = term.act( kitty_apc( "a=T,i=90,c=2147483647,r=1,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check_eq( reply, "\033_Gi=90;EINVAL:parameter out of range\033\\", "c out of range rejects the whole command" );
    check( term.get_fb().image_store_resolve( 90 ) == 0, "out-of-range c stores nothing" );
  }

  /* C=1 does not scroll, anchors at the cursor row exactly, keeps the full
     computed row count, and leaves the cursor untouched. */
  {
    Complete term( 80, 24 );
    term.act( "mark" );       /* row 0 now has "mark" at columns 0-3 */
    term.act( "\033[24;1H" ); /* cursor to the bottom row (row 23), column 0 */
    std::string png = minimal_png( 100, 100 );
    term.act( kitty_apc( "a=T,i=120,f=100,r=10,C=1", base64_encode( png ) ) );

    std::string ch;
    term.get_fb().get_row( 0 )->cells[0].print_grapheme( ch );
    check( ch == "m", "C=1 does not scroll: row 0 text is unchanged" );

    uint32_t internal = term.get_fb().image_store_resolve( 120 );
    bool found = false;
    for ( const auto& p : term.get_fb().get_row( 23 )->placements ) {
      if ( p->internal_image_id == internal ) {
        found = true;
        check( p->rows == 10, "C=1 keeps the full computed row count, unclamped to the screen" );
      }
    }
    check( found, "C=1 anchors the placement at the cursor row, not scrolled" );
    check( term.get_fb().ds.get_cursor_row() == 23 && term.get_fb().ds.get_cursor_col() == 0,
           "C=1 leaves the cursor untouched" );
  }

  /* An empty or wrongly-sized payload on an actual transmit is rejected, so
     a=t,f=24,s=1,v=1 with no payload cannot mint free images. */
  {
    Complete term( 80, 24 );
    check_eq( term.act( kitty_apc( "a=t,i=100,f=24,s=1,v=1" ) ),
              "\033_Gi=100;EINVAL:empty payload\033\\",
              "empty payload on a=t is rejected" );
    check( term.get_fb().image_store_resolve( 100 ) == 0, "empty payload creates no image" );

    check_eq( term.act( kitty_apc( "a=t,i=101,f=24,s=2,v=2", base64_encode( std::string( 6, 'x' ) ) ) ),
              "\033_Gi=101;EINVAL:payload size does not match dimensions\033\\",
              "a payload shorter than s*v*3 is rejected" );
    check( term.get_fb().image_store_resolve( 101 ) == 0, "wrong-size payload creates no image" );

    check_eq( term.act( kitty_apc( "a=t,i=102,f=24,s=1,v=1", base64_encode( std::string( 3, 'x' ) ) ) ),
              "\033_Gi=102;OK\033\\",
              "a correctly-sized payload still succeeds" );
  }

  /* The image store is capped at 256 images, replying ENOSPC beyond that
     once nothing more can be evicted (every image here is placed, so none
     of them are evictable). */
  {
    Complete term( 80, 24 );
    for ( int i = 1; i <= 256; i++ ) {
      std::string id = std::to_string( 3000 + i );
      check_eq( term.act( kitty_apc( "a=T,i=" + id + ",f=32,s=1,v=1", base64_encode( std::string( 4, 'z' ) ) ) ),
                "\033_Gi=" + id + ";OK\033\\",
                "image count cap: image " + id + " stored" );
    }
    check( term.get_fb().image_store_count() == 256, "image count cap: exactly 256 images stored" );
    check_eq( term.act( kitty_apc( "a=T,i=9999,f=32,s=1,v=1", base64_encode( std::string( 4, 'z' ) ) ) ),
              "\033_Gi=9999;ENOSPC\033\\",
              "image count cap: the 257th placed image replies ENOSPC" );
  }

  /* The total placement list is capped following the screen's own area
     (80*24 = 1920 here), independent of the image count and byte caps --
     repeated a=p must not grow it forever. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=t,i=200,f=32,s=1,v=1", base64_encode( std::string( 4, 'z' ) ) ) );
    for ( int p = 1; p <= 1920; p++ ) {
      std::string ps = std::to_string( p );
      check_eq( term.act( kitty_apc( "a=p,i=200,p=" + ps ) ),
                "\033_Gi=200,p=" + ps + ";OK\033\\",
                "placement cap: placement " + ps + " added" );
    }
    check( term.get_fb().placement_count() == 1920, "placement cap: exactly 1920 placements exist" );
    check_eq( term.act( kitty_apc( "a=p,i=200,p=1921" ) ),
              "\033_Gi=200,p=1921;ENOSPC\033\\",
              "placement cap: the 1921st placement replies ENOSPC" );
  }

  /* Cap follows a larger screen: yazi's old-style per-cell Kitty driver
     (used whenever it cannot identify the terminal brand) emits one a=p per
     cell of the preview with a source rectangle. At 147x40 the cap is
     147*40 = 5880, comfortably above the 2774 placements a 73x38 pane
     wants -- this is the regression that motivated the cap following the
     screen instead of a flat 1024. */
  {
    Complete term( 147, 40 );
    term.act( kitty_apc( "a=t,i=300,f=32,s=1,v=1", base64_encode( std::string( 4, 'z' ) ) ) );
    for ( int p = 1; p <= 2774; p++ ) {
      std::string ps = std::to_string( p );
      check_eq( term.act( kitty_apc( "a=p,i=300,p=" + ps + ",x=0,y=0,w=1,h=1,c=1,r=1,z=-1,C=1" ) ),
                "\033_Gi=300,p=" + ps + ";OK\033\\",
                "yazi per-cell placement: placement " + ps + " added" );
    }
    check( term.get_fb().placement_count() == 2774, "yazi per-cell placement: all 2774 placements stored" );
  }

  /* Floor holds on a tiny screen: 20x5 has area 100, well under
     PLACEMENT_CAP_FLOOR, so the cap stays at 1024. */
  {
    Complete term( 20, 5 );
    check( term.get_fb().placement_cap() == 1024, "placement cap: floor holds on a tiny screen" );
  }

  /* Ceiling holds: 300x250 has area 75000, above PLACEMENT_CAP_CEILING, so
     the cap is clamped to 65536. Assert on the accessor only -- actually
     emitting that many placements would slow the test for no benefit. */
  {
    Complete term( 300, 250 );
    check( term.get_fb().placement_cap() == 65536, "placement cap: ceiling holds on a huge screen" );
  }

  /* Exact area in the ordinary case: no flooring or ceiling in play. */
  {
    Complete term( 147, 40 );
    check( term.get_fb().placement_cap() == 5880, "placement cap: exact screen area in the ordinary case" );
  }

  /* Shrinking the terminal lowers the cap but never prunes existing
     placements, so placement_count() can end up above the new cap. A
     replacement of an existing placement id must still be let through in
     that state (it does not grow the count); only a genuinely new
     placement id is refused. 50x24 = 1200 is picked for the resized cap so
     it lands strictly between PLACEMENT_CAP_FLOOR and the 1500 placements
     already stored -- the point being tested is the cap following the
     screen down to its own area, not the floor clamp. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=t,i=400,f=32,s=1,v=1", base64_encode( std::string( 4, 'z' ) ) ) );
    for ( int p = 1; p <= 1500; p++ ) {
      std::string ps = std::to_string( p );
      check_eq( term.act( kitty_apc( "a=p,i=400,p=" + ps ) ),
                "\033_Gi=400,p=" + ps + ";OK\033\\",
                "shrink-then-replace setup: placement " + ps + " added at 80x24" );
    }
    check( term.get_fb().placement_count() == 1500, "shrink-then-replace setup: 1500 placements exist" );

    term.act( Parser::Resize( 50, 24 ) );
    check( term.get_fb().placement_cap() == 1200, "shrink-then-replace: cap follows the screen down to 1200" );
    check( term.get_fb().placement_count() == 1500,
           "shrink-then-replace: shrinking does not prune existing placements" );

    check_eq( term.act( kitty_apc( "a=p,i=400,p=1" ) ),
              "\033_Gi=400,p=1;OK\033\\",
              "shrink-then-replace: replacing an existing id succeeds despite count over the new cap" );
    check( term.get_fb().placement_count() == 1500,
           "shrink-then-replace: replacing an existing id neither grows nor shrinks the list" );
    check_eq( term.act( kitty_apc( "a=p,i=400,p=1501" ) ),
              "\033_Gi=400,p=1501;ENOSPC\033\\",
              "shrink-then-replace: a brand new id is still refused over the new cap" );
  }

  /* An a=p (or a=T) with a nonzero p that already exists for that image
     replaces the old placement instead of adding a new one. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=t,i=210,f=32,s=1,v=1", base64_encode( std::string( 4, 'z' ) ) ) );

    term.act( "\033[5;1H" ); /* row 4 */
    term.act( kitty_apc( "a=p,i=210,p=7" ) );
    check( term.get_fb().get_row( 4 )->placements.size() == 1, "first placement with p=7 lands at row 4" );

    term.act( "\033[10;1H" ); /* row 9 */
    term.act( kitty_apc( "a=p,i=210,p=7" ) );
    check( term.get_fb().get_row( 4 )->placements.empty(), "re-placing p=7 removes the old placement" );
    check( term.get_fb().get_row( 9 )->placements.size() == 1, "re-placing p=7 adds the new placement at row 9" );
    check( term.get_fb().placement_count() == 1, "re-placing p=7 nets zero placement growth" );
  }

  /* do_transmit must credit the replaced image's own bytes before deciding
     whether a re-transmit of the same id fits. */
  {
    Complete term( 80, 24 );
    Framebuffer::set_kitty_store_cap_for_tests( 2000 ); /* exactly one 2000-byte image */
    std::string img( 2000, 'z' );                       /* f=32, s=1, v=500 -> 2000 bytes */

    check_eq( term.act( kitty_apc( "a=t,i=220,f=32,s=1,v=500", base64_encode( img ) ) ),
              "\033_Gi=220;OK\033\\",
              "a cap-filling image stores" );
    check_eq( term.act( kitty_apc( "a=t,i=220,f=32,s=1,v=500", base64_encode( img ) ) ),
              "\033_Gi=220;OK\033\\",
              "re-transmitting the same id at full capacity still succeeds" );

    Framebuffer::set_kitty_store_cap_for_tests( 64 * 1024 * 1024 ); /* restore the default for later tests */
  }

  /* I= with no i= allocates a server-side app id from a counter starting at
     0x80000000, maps I -> that id, and a=p/a=d resolve through the map. */
  {
    Complete term( 80, 24 );
    check_eq( term.act( kitty_apc( "a=T,I=3,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) ),
              "\033_Gi=2147483648,I=3;OK\033\\",
              "a=T,I=3 replies with I=3 and an allocated i=" );

    check_eq( term.act( kitty_apc( "a=p,I=3" ) ), "\033_GI=3;OK\033\\", "a=p,I=3 resolves through the number map" );
    check( term.get_fb().placement_count() == 2, "a=T and a=p,I=3 both placed the same image" );

    check_eq(
      term.act( kitty_apc( "a=d,d=i,I=3" ) ), "\033_GI=3;OK\033\\", "a=d,d=i,I=3 resolves through the number map" );
    check( term.get_fb().placement_count() == 0, "a=d,d=i,I=3 removed every placement of the resolved image" );
  }

  /* a=d,d=I frees the image's data only once no placement of it remains,
     not as soon as any one placement (e.g. one scoped by p=) is removed. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=230,f=24,s=1,v=1,p=1", base64_encode( std::string( 3, '\0' ) ) ) );
    term.act( "\033[3;1H" );
    term.act( kitty_apc( "a=p,i=230,p=2" ) );
    check( term.get_fb().placement_count() == 2, "two placements of image 230 exist" );

    term.act( kitty_apc( "a=d,d=I,i=230,p=1" ) );
    check( term.get_fb().placement_count() == 1, "one placement of image 230 remains after deleting p=1" );
    check( term.get_fb().image_store_resolve( 230 ) != 0,
           "data kept: another placement still references the image" );

    term.act( kitty_apc( "a=d,d=I,i=230,p=2" ) );
    check( term.get_fb().placement_count() == 0, "no placements of image 230 remain" );
    check( term.get_fb().image_store_resolve( 230 ) == 0, "data freed once no placement references the image" );
  }

  /* A bare a=d (no d= key at all) behaves as d=a, per the Kitty default. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=240,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "placement exists before bare a=d" );

    term.act( kitty_apc( "a=d" ) );
    check( term.get_fb().get_row( 0 )->placements.empty(), "bare a=d clears placements like d=a" );
    check( term.get_fb().image_store_resolve( 240 ) != 0, "bare a=d (like d=a) keeps image data" );
  }

  /* a=d,d=A clears the image-number map along with the image store. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,I=10,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    term.act( kitty_apc( "a=T,I=11,f=24,s=1,v=1", base64_encode( std::string( 3, '\x01' ) ) ) );
    check( term.get_fb().image_store_resolve_number( 10 ) != 0, "number 10 resolves before d=A" );
    check( term.get_fb().image_store_resolve_number( 11 ) != 0, "number 11 resolves before d=A" );

    term.act( kitty_apc( "a=d,d=A" ) );
    check( term.get_fb().image_store_resolve_number( 10 ) == 0, "number 10 mapping is gone after d=A" );
    check( term.get_fb().image_store_resolve_number( 11 ) == 0, "number 11 mapping is gone after d=A" );
  }

  /* Re-transmitting an id that had a number mapping drops the stale
     mapping to the old (now-replaced) image. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=300,I=20,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) );
    uint32_t internal1 = term.get_fb().image_store_resolve( 300 );
    check( term.get_fb().image_store_resolve_number( 20 ) == internal1, "I=20 maps to the first image" );

    term.act( kitty_apc( "a=T,i=300,f=24,s=1,v=1", base64_encode( std::string( 3, '\x01' ) ) ) );
    uint32_t internal2 = term.get_fb().image_store_resolve( 300 );
    check( internal2 != internal1, "re-transmitting i=300 allocates a new internal id" );
    check( term.get_fb().image_store_resolve_number( 20 ) == 0,
           "the stale I=20 mapping to the replaced image is gone" );
  }

  /* A key that fails to parse (out of range, or trailing garbage after the
     digits) makes the whole command reply EINVAL rather than truncating or
     invoking undefined behaviour. A second, valid id key is included so the
     failure is actually observable as a reply. */
  {
    Complete term( 80, 24 );
    check_eq( term.act( kitty_apc( "a=T,i=99999999999999999999,I=50,f=24,s=1,v=1",
                                   base64_encode( std::string( 3, '\0' ) ) ) ),
              "\033_GI=50;EINVAL:invalid parameter\033\\",
              "an out-of-range i replies EINVAL" );
    check( term.get_fb().image_store_resolve_number( 50 ) == 0, "the rejected command stored nothing" );

    check_eq( term.act( kitty_apc( "a=T,i=91,c=12abc,r=1,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) ),
              "\033_Gi=91;EINVAL:invalid parameter\033\\",
              "trailing garbage after a numeric key replies EINVAL" );
    check( term.get_fb().image_store_resolve( 91 ) == 0, "the rejected command stored nothing" );
  }

  /* An auto-allocated app id for I= must not collide with (and silently
     destroy) an app-chosen i= that happens to land in the allocator's
     range; it skips ahead to the next free id instead. */
  {
    Complete term( 80, 24 );
    check_eq( term.act( kitty_apc( "a=T,i=2147483648,f=24,s=1,v=1", base64_encode( std::string( 3, '\0' ) ) ) ),
              "\033_Gi=2147483648;OK\033\\",
              "an explicit id at the allocator's starting value stores" );
    uint32_t internal1 = term.get_fb().image_store_resolve( 2147483648u );
    check( internal1 != 0, "the explicit-id image is stored" );
    check( term.get_fb().get_row( 0 )->placements.size() == 1, "the explicit-id image is placed" );

    check_eq( term.act( kitty_apc( "a=T,I=5,f=24,s=1,v=1", base64_encode( std::string( 3, '\x01' ) ) ) ),
              "\033_Gi=2147483649,I=5;OK\033\\",
              "the allocator skips the colliding id and assigns the next free one" );

    check( term.get_fb().image_store_resolve( 2147483648u ) == internal1,
           "the first (explicit-id) image's mapping survives untouched" );
    uint32_t internal2 = term.get_fb().image_store_resolve( 2147483649u );
    check( ( internal2 != 0 ) && ( internal2 != internal1 ), "the auto-allocated id maps to a distinct new image" );
    check( term.get_fb().get_row( 0 )->placements.size() == 2, "both placements survive" );
  }

  /* A placement taller than the screen stores its full computed extent
     (renderers clip it); only the scroll amount is clamped to the screen
     height. */
  {
    Complete term( 80, 24 );
    std::string png = minimal_png( 50, 50 );
    term.act( kitty_apc( "a=T,i=130,f=100,r=100", base64_encode( png ) ) );

    uint32_t internal = term.get_fb().image_store_resolve( 130 );
    bool found = false;
    for ( const auto& p : term.get_fb().get_row( 0 )->placements ) {
      if ( p->internal_image_id == internal ) {
        found = true;
        check( p->rows == 100, "stores the full unclamped row count (100), not the screen height" );
      }
    }
    check( found, "a 100-row placement on a 24-row screen anchors at row 0" );
    check( term.get_fb().ds.get_cursor_row() == 23,
           "the scroll amount is clamped to the screen height (cursor ends at the bottom row)" );
  }

  /* --- Wire: carrying images and placements from a server Complete to a
     client Complete via diff_from/apply_string, paced in small batches
     (docs/notes/20260908T153303--kitty-graphics-over-the-state-sync-stream). --- */

  /* A 100 KiB image, admitted in four 32 KiB batches: the first diff carries
     the placement and the first batch; the client ends up with an image
     entry with the right metadata, pending until the last diff; after the
     last diff the client's blob matches the server's byte for byte and its
     placement matches the server's uid, columns and rows. Also proves
     diff_from is deterministic for a fixed state pair. */
  {
    Complete server( 80, 24 );

    const size_t total_bytes = 100 * 1024;
    std::string png = minimal_png( 50, 50, total_bytes - 24 );
    check( png.size() == total_bytes, "wire: fake PNG is exactly 100 KiB" );

    std::string reply = kitty_chunked_transmit( server, "a=T,i=500,f=100", png );
    check_eq( reply, "\033_Gi=500;OK\033\\", "wire: chunked a=T on the server replies OK" );
    check( server.get_fb().get_row( 0 )->placements.size() == 1, "wire: server has one placement" );
    const uint32_t internal_id = server.get_fb().get_row( 0 )->placements[0]->internal_image_id;
    const uint32_t server_uid = server.get_fb().get_row( 0 )->placements[0]->uid;

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();

    Complete prev( 80, 24 ); /* baseline: what the client currently has -- nothing yet */

    /* Round 1: first 32 KiB batch, plus the placement. */
    server.admit_image_bytes( 32768 );
    std::string diff1 = server.diff_from( prev );

    HostBuffers::HostMessage msg1;
    check( msg1.ParseFromString( diff1 ), "wire: diff1 parses as a HostMessage" );
    bool found_chunk = false;
    for ( int i = 0; i < msg1.instruction_size(); i++ ) {
      if ( msg1.instruction( i ).HasExtension( HostBuffers::imagechunk ) ) {
        const auto& chunk = msg1.instruction( i ).GetExtension( HostBuffers::imagechunk );
        check( chunk.image_id() == internal_id, "wire: first diff's chunk is for the placed image" );
        check( chunk.offset() == 0, "wire: first diff's chunk starts at offset 0" );
        check( chunk.data().size() == 32768, "wire: first diff's chunk carries exactly the first 32 KiB" );
        check( chunk.total() == total_bytes, "wire: first diff's chunk states the full 100 KiB total" );
        found_chunk = true;
      }
    }
    check( found_chunk, "wire: first diff carries an image chunk" );
    check( diff_hostbytes_contains( msg1, "\033_Ga=p" ), "wire: first diff carries the placement" );

    check_eq( server.diff_from( prev ), diff1, "wire: diff_from is deterministic for the same state pair" );

    client.apply_string( diff1 );

    std::shared_ptr<const Image> client_image = client.get_fb().image_store_get( internal_id );
    check( client_image != nullptr, "wire: client created an image entry from the first chunk" );
    if ( client_image ) {
      check( client_image->format == 100 && client_image->width == 50 && client_image->height == 50,
             "wire: client's image entry has the server's metadata" );
      check( !client_image->blob, "wire: client's image entry is still pending after the first batch" );
    }
    check( client.get_fb().get_row( 0 )->placements.size() == 1,
           "wire: client has one placement after the first diff" );
    if ( client.get_fb().get_row( 0 )->placements.size() == 1 ) {
      const auto& p = client.get_fb().get_row( 0 )->placements[0];
      check( p->uid == server_uid && p->internal_image_id == internal_id,
             "wire: client's placement matches the server's uid and internal id" );
    }

    prev = server; /* the client is now assumed to have round 1's state */

    /* Rounds 2 and 3: two more 32 KiB batches; still not complete. */
    for ( int round = 0; round < 2; round++ ) {
      server.admit_image_bytes( 32768 );
      std::string diff = server.diff_from( prev );
      client.apply_string( diff );
      prev = server;
    }
    std::shared_ptr<const Image> client_image_mid = client.get_fb().image_store_get( internal_id );
    check( client_image_mid && !client_image_mid->blob, "wire: still pending after three 32 KiB batches" );

    /* Round 4: one more 32 KiB budget, but only the remainder is left to
       admit -- this completes the image. */
    server.admit_image_bytes( 32768 );
    std::string diff4 = server.diff_from( prev );
    client.apply_string( diff4 );

    std::shared_ptr<const Image> server_image = server.get_fb().image_store_get( internal_id );
    std::shared_ptr<const Image> client_image_final = client.get_fb().image_store_get( internal_id );
    check( server_image && server_image->blob, "wire: server's image is complete" );
    check( client_image_final && client_image_final->blob, "wire: client's image is complete after the last diff" );
    if ( server_image && server_image->blob && client_image_final && client_image_final->blob ) {
      check( *client_image_final->blob == *server_image->blob,
             "wire: client's blob matches the server's byte for byte" );
    }

    check( client.get_fb().get_row( 0 )->placements.size() == 1, "wire: client still has exactly one placement" );
    if ( client.get_fb().get_row( 0 )->placements.size() == 1
         && server.get_fb().get_row( 0 )->placements.size() == 1 ) {
      const auto& cp = client.get_fb().get_row( 0 )->placements[0];
      const auto& sp = server.get_fb().get_row( 0 )->placements[0];
      check( cp->uid == sp->uid && cp->columns == sp->columns && cp->rows == sp->rows,
             "wire: client's placement has the server's uid, columns and rows" );
    }
  }

  /* Ordering: a placement must never reach the client before the image
     entry it names, even with zero bytes admitted -- admission can lag
     well behind a=T (an earlier image still being paced, or a state simply
     sent before admission next runs). */
  {
    Complete server( 80, 24 );
    std::string pixels( 12, '\x77' );
    server.act( kitty_apc( "a=T,i=900,f=24,s=2,v=2", base64_encode( pixels ) ) );
    check( server.get_fb().get_row( 0 )->placements.size() == 1, "wire ordering: server has a placement" );
    const uint32_t internal_id = server.get_fb().get_row( 0 )->placements[0]->internal_image_id;

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    /* No admission at all before this diff. */
    std::string diff1 = server.diff_from( prev );
    client.apply_string( diff1 );

    std::shared_ptr<const Image> client_image = client.get_fb().image_store_get( internal_id );
    check( client_image != nullptr, "wire ordering: client created the image entry with zero bytes admitted" );
    if ( client_image ) {
      check( client_image->format == 24 && client_image->width == 2 && client_image->height == 2,
             "wire ordering: the zero-byte entry still carries the server's metadata" );
      check( !client_image->blob, "wire ordering: the zero-byte entry is pending" );
    }
    check( client.get_fb().get_row( 0 )->placements.size() == 1,
           "wire ordering: the placement is present on the client (not dropped as ENOENT)" );

    prev = server;
    server.admit_image_bytes( 32768 ); /* covers the whole 12-byte blob */
    client.apply_string( server.diff_from( prev ) );

    std::shared_ptr<const Image> client_image2 = client.get_fb().image_store_get( internal_id );
    std::shared_ptr<const Image> server_image = server.get_fb().image_store_get( internal_id );
    check( client_image2 && client_image2->blob, "wire ordering: second diff completes the blob" );
    if ( client_image2 && client_image2->blob && server_image && server_image->blob ) {
      check( *client_image2->blob == *server_image->blob, "wire ordering: completed blob matches the server's" );
    }
  }

  /* A chunk whose offset doesn't match what the client already has (data
     from the network, so this covers an out-of-sync or misbehaving peer,
     not a real diff_from bug) must be dropped with no state change, not
     abort the client. Proven indirectly: if the bad chunk were appended
     anyway, the pending byte count would be wrong and the next (correct)
     chunk's offset would then also fail to line up, so the image would
     never complete or would complete with the wrong bytes. */
  {
    Framebuffer fb( 80, 24 );

    auto piece1 = std::make_shared<const std::string>( std::string( 5, 'x' ) );
    fb.kitty_apply_chunk( 910, 0, piece1, 10, 24, 1, 1, false );
    std::shared_ptr<const Image> pending = fb.image_store_get( 910 );
    check( pending != nullptr && !pending->blob,
           "wire tolerant offset: first (correct) chunk leaves the image pending" );

    auto bad_piece = std::make_shared<const std::string>( std::string( 3, 'y' ) );
    fb.kitty_apply_chunk( 910, 999, bad_piece, 10, 24, 1, 1, false ); /* offset should be 5, not 999 */
    std::shared_ptr<const Image> still_pending = fb.image_store_get( 910 );
    check( still_pending != nullptr && !still_pending->blob,
           "wire tolerant offset: wrong-offset chunk does not complete or corrupt the image" );

    auto piece2 = std::make_shared<const std::string>( std::string( 5, 'z' ) );
    fb.kitty_apply_chunk( 910, 5, piece2, 10, 24, 1, 1, false ); /* the real next piece, at the real offset */
    std::shared_ptr<const Image> done = fb.image_store_get( 910 );
    check( done != nullptr && static_cast<bool>( done->blob ),
           "wire tolerant offset: the real continuation completes the image" );
    if ( done && done->blob ) {
      check(
        *done->blob == ( std::string( 5, 'x' ) + std::string( 5, 'z' ) ),
        "wire tolerant offset: the wrong-offset chunk was dropped, final bytes are exactly the two good pieces" );
    }
  }

  /* Scroll: once the placement is complete on both sides, line feeds that
     scroll the server's screen move its row (and placement) with it, via
     the same shared_ptr<Row> mechanism plain text scrolling already uses --
     no a=p is re-emitted, and the client's placement row matches. */
  {
    Complete server( 80, 24 );
    /* Distinct text on every row, so the scroll shortcut's row-by-row
       content match is genuine rather than a coincidental match between two
       otherwise-untouched blank rows (which all still share one identical
       "blank row" object and would defeat the shortcut's own row-0 early-out,
       an unrelated pre-existing limitation this test sidesteps rather than
       exercises). A real screen has text, so this is the realistic case. */
    for ( int r = 0; r < 24; r++ ) {
      server.act( "\033[" + std::to_string( r + 1 ) + ";1H" );
      server.act( std::string( 1, static_cast<char>( 'A' + ( r % 26 ) ) ) );
    }
    server.act( "\033[11;1H" ); /* row index 10, column 0 */
    std::string pixels( 12, '\x33' );
    server.act( kitty_apc( "a=T,i=600,f=24,s=2,v=2", base64_encode( pixels ) ) );
    check( server.get_fb().get_row( 10 )->placements.size() == 1, "wire scroll: placement created at row 10" );
    const uint32_t internal_id = server.get_fb().get_row( 10 )->placements[0]->internal_image_id;
    const uint32_t uid = server.get_fb().get_row( 10 )->placements[0]->uid;

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    server.admit_image_bytes( 32768 ); /* covers the whole 12-byte blob in one batch */
    client.apply_string( server.diff_from( prev ) );
    prev = server;

    std::shared_ptr<const Image> server_img = server.get_fb().image_store_get( internal_id );
    std::shared_ptr<const Image> client_img = client.get_fb().image_store_get( internal_id );
    check( server_img && server_img->blob && client_img && client_img->blob
             && ( *server_img->blob == *client_img->blob ),
           "wire scroll: image complete and matching on both sides before scrolling" );
    check( client.get_fb().get_row( 10 )->placements.size() == 1
             && client.get_fb().get_row( 10 )->placements[0]->uid == uid,
           "wire scroll: client's placement lands at row 10 too" );

    server.act( "\033[24;1H" ); /* bottom row */
    server.act( "\n" );         /* scroll the whole screen up by 1 */
    check( server.get_fb().get_row( 9 )->placements.size() == 1, "wire scroll: server placement moved to row 9" );

    std::string diff2 = server.diff_from( prev );
    HostBuffers::HostMessage msg2;
    check( msg2.ParseFromString( diff2 ), "wire scroll: diff2 parses" );
    check( !diff_hostbytes_contains( msg2, "\033_Ga=p" ), "wire scroll: the scroll diff contains no a=p" );

    client.apply_string( diff2 );
    check( client.get_fb().get_row( 9 )->placements.size() == 1
             && client.get_fb().get_row( 9 )->placements[0]->uid == uid,
           "wire scroll: client's placement row matches the server's after scrolling" );
  }

  /* Delete: a=d,d=a on the server produces a diff carrying a=d; the
     client's placement disappears, but its store keeps the image data,
     matching the server (which also keeps the data on d=a). */
  {
    Complete server( 80, 24 );
    std::string pixels( 12, '\x44' );
    server.act( kitty_apc( "a=T,i=700,f=24,s=2,v=2", base64_encode( pixels ) ) );
    const uint32_t internal_id = server.get_fb().get_row( 0 )->placements[0]->internal_image_id;

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );
    prev = server;
    check( client.get_fb().get_row( 0 )->placements.size() == 1,
           "wire delete: client has the placement before delete" );

    server.act( kitty_apc( "a=d,d=a" ) );
    check( server.get_fb().get_row( 0 )->placements.empty(), "wire delete: server placement gone" );

    std::string diff2 = server.diff_from( prev );
    HostBuffers::HostMessage msg2;
    check( msg2.ParseFromString( diff2 ), "wire delete: diff2 parses" );
    check( diff_hostbytes_contains( msg2, "\033_Ga=d" ), "wire delete: the diff carries a=d" );

    client.apply_string( diff2 );
    check( client.get_fb().get_row( 0 )->placements.empty(), "wire delete: client's placement is gone" );
    std::shared_ptr<const Image> client_image = client.get_fb().image_store_get( internal_id );
    check( client_image != nullptr && static_cast<bool>( client_image->blob ),
           "wire delete: client's store still has the image data" );
  }

  /* Re-transmit: the server replacing app id 800 with a new image (a new
     internal id) results in the client placing the new internal id and
     dropping the old placement -- exactly one placement, naming the new
     id, with the new bytes. */
  {
    Complete server( 80, 24 );
    std::string pixels1( 12, '\x11' );
    server.act( kitty_apc( "a=T,i=800,f=24,s=2,v=2", base64_encode( pixels1 ) ) );
    const uint32_t internal1 = server.get_fb().get_row( 0 )->placements[0]->internal_image_id;

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );
    prev = server;
    check( client.get_fb().get_row( 0 )->placements.size() == 1
             && client.get_fb().get_row( 0 )->placements[0]->internal_image_id == internal1,
           "wire retransmit: client placed the first image" );

    std::string pixels2( 12, '\x22' );
    server.act( kitty_apc( "a=T,i=800,f=24,s=2,v=2", base64_encode( pixels2 ) ) );
    const uint32_t internal2 = server.get_fb().get_row( 0 )->placements[0]->internal_image_id;
    check( internal2 != internal1, "wire retransmit: server allocated a new internal id" );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );

    check( client.get_fb().get_row( 0 )->placements.size() == 1
             && client.get_fb().get_row( 0 )->placements[0]->internal_image_id == internal2,
           "wire retransmit: client ends up with only the new internal id placed" );
    std::shared_ptr<const Image> client_image2 = client.get_fb().image_store_get( internal2 );
    check( client_image2 && client_image2->blob && ( *client_image2->blob == pixels2 ),
           "wire retransmit: client's new image data matches" );
  }

  /* The client's store must not grow without bound: an image the server
     frees (d=A here) or drops via a same-id re-transmit produces only a=d
     in hostbytes, which removes the placement but not the client's copy
     of the pixels. */
  {
    Complete server( 80, 24 );
    const size_t total_bytes = 100000;
    std::string png = minimal_png( 50, 50, total_bytes - 24 );
    std::string reply = kitty_chunked_transmit( server, "a=T,i=980,f=100", png );
    check_eq( reply, "\033_Gi=980;OK\033\\", "wire remove: chunked a=T on the server replies OK" );
    const uint32_t internal_id = server.get_fb().image_store_resolve( 980 );
    check( internal_id != 0, "wire remove: image stored on the server" );

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    server.admit_image_bytes( 32768 ); /* partial: well short of total_bytes */
    client.apply_string( server.diff_from( prev ) );
    prev = server;

    std::shared_ptr<const Image> client_image = client.get_fb().image_store_get( internal_id );
    check( client_image != nullptr && !client_image->blob,
           "wire remove: client has a pending (incomplete) copy before removal" );

    server.act( kitty_apc( "a=d,d=A" ) );
    check( server.get_fb().image_store_resolve( 980 ) == 0, "wire remove: server's own store forgot the image" );

    std::string diff2 = server.diff_from( prev );
    HostBuffers::HostMessage msg2;
    check( msg2.ParseFromString( diff2 ), "wire remove: diff2 parses" );
    bool found_remove = false;
    for ( int i = 0; i < msg2.instruction_size(); i++ ) {
      if ( msg2.instruction( i ).HasExtension( HostBuffers::imagechunk )
           && msg2.instruction( i ).GetExtension( HostBuffers::imagechunk ).remove()
           && msg2.instruction( i ).GetExtension( HostBuffers::imagechunk ).image_id() == internal_id ) {
        found_remove = true;
      }
    }
    check( found_remove, "wire remove: the diff carries a remove instruction for the image" );

    client.apply_string( diff2 );
    check( client.get_fb().image_store_get( internal_id ) == nullptr,
           "wire remove: the diff removes the image from the client's store" );
    check( client.get_fb().get_row( 0 )->placements.empty(), "wire remove: the client's placement is also gone" );
  }

  /* Direct proof that image_store_forget (what the remove-chunk path
     calls) drops pending pieces too, not just the image entry: after
     forgetting a still-assembling id, a chunk at offset 0 for that same id
     starts a fresh assembly instead of being rejected as misaligned
     against stale leftover pieces (real ids are never reused; this is a
     white-box check of the store function itself). */
  {
    Framebuffer fb( 80, 24 );
    auto piece1 = std::make_shared<const std::string>( std::string( 4, 'a' ) );
    fb.kitty_apply_chunk( 990, 0, piece1, 8, 24, 1, 1, false );
    check( fb.image_store_get( 990 ) != nullptr, "wire remove: pending entry exists before forget" );

    fb.image_store_forget( 990 );
    check( fb.image_store_get( 990 ) == nullptr, "wire remove: image_store_forget removes the entry" );

    auto piece2 = std::make_shared<const std::string>( std::string( 4, 'b' ) );
    fb.kitty_apply_chunk( 990, 0, piece2, 4, 24, 1, 1, false ); /* offset 0: only accepted if pending was cleared */
    std::shared_ptr<const Image> fresh = fb.image_store_get( 990 );
    check( fresh != nullptr && fresh->blob && ( *fresh->blob == std::string( 4, 'b' ) ),
           "wire remove: image_store_forget drops pending pieces too, offset 0 starts fresh" );
  }

  /* A replacement of the same app id (i=) removes the old internal id from
     the client's store, not just from the row it was placed on. */
  {
    Complete server( 80, 24 );
    std::string pixels1( 12, '\x33' );
    server.act( kitty_apc( "a=T,i=985,f=24,s=2,v=2", base64_encode( pixels1 ) ) );
    const uint32_t internal1 = server.get_fb().image_store_resolve( 985 );

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );
    prev = server;
    check( client.get_fb().image_store_get( internal1 ) != nullptr,
           "wire remove: client has the first image before re-transmit" );

    std::string pixels2( 12, '\x44' );
    server.act( kitty_apc( "a=T,i=985,f=24,s=2,v=2", base64_encode( pixels2 ) ) );
    const uint32_t internal2 = server.get_fb().image_store_resolve( 985 );
    check( internal2 != internal1, "wire remove: re-transmit allocates a new internal id" );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );

    check( client.get_fb().image_store_get( internal2 ) != nullptr, "wire remove: client has the new image" );
    check( client.get_fb().image_store_get( internal1 ) == nullptr,
           "wire remove: re-transmit removes the old internal id from the client's store" );
  }

  /* Client-side cap defence: a chunk that promises more than the client
     will ever hold (either cap) is refused outright, before creating any
     entry -- the client never evicts, unlike the server. */
  {
    Framebuffer fb( 80, 24 );
    Framebuffer::set_kitty_store_cap_for_tests( 100 );

    auto oversized_total = std::make_shared<const std::string>( std::string( 50, 'z' ) );
    fb.kitty_apply_chunk( 995, 0, oversized_total, 200 /* exceeds the 100-byte store cap */, 24, 1, 1, false );
    check( fb.image_store_get( 995 ) == nullptr,
           "wire remove: a chunk whose total exceeds the store cap creates no entry" );

    auto over_per_image_cap = std::make_shared<const std::string>( std::string( 4, 'x' ) );
    fb.kitty_apply_chunk(
      996, 0, over_per_image_cap, Kitty::IMAGE_MAX_BYTES + 1, 24, 1, 1, false ); /* exceeds the per-image cap */
    check( fb.image_store_get( 996 ) == nullptr,
           "wire remove: a chunk whose total exceeds IMAGE_MAX_BYTES creates no entry" );

    auto within_cap = std::make_shared<const std::string>( std::string( 4, 'a' ) );
    fb.kitty_apply_chunk( 997, 0, within_cap, 4, 24, 1, 1, false );
    check( fb.image_store_get( 997 ) != nullptr, "wire remove: a chunk within both caps still creates an entry" );

    Framebuffer::set_kitty_store_cap_for_tests( 64 * 1024 * 1024 ); /* restore the default for later tests */
  }

  /* RIS must not restart the internal-id or placement-uid counters: the
     transport keeps older states around that can still name images and
     placements by id, so restarting at 1 risks a post-RIS image reusing an
     id the client already has complete -- its chunks would then be ignored
     as stale. */
  {
    Complete server( 80, 24 );
    std::string pixels1( 12, '\x11' );
    server.act( kitty_apc( "a=T,i=950,f=24,s=2,v=2", base64_encode( pixels1 ) ) );
    const uint32_t internal1 = server.get_fb().image_store_resolve( 950 );
    check( internal1 != 0, "reset identity: first image stored before RIS" );

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );
    prev = server;
    std::shared_ptr<const Image> client_image1 = client.get_fb().image_store_get( internal1 );
    check( client_image1 && client_image1->blob, "reset identity: client has the first image complete before RIS" );

    server.act( "\033c" ); /* RIS: full reset */
    std::string pixels2( 12, '\x22' );
    server.act( kitty_apc( "a=T,i=950,f=24,s=2,v=2", base64_encode( pixels2 ) ) );
    const uint32_t internal2 = server.get_fb().image_store_resolve( 950 );
    check( internal2 != 0 && internal2 != internal1,
           "reset identity: second image (post-RIS) gets a fresh internal id" );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );

    std::shared_ptr<const Image> client_image2 = client.get_fb().image_store_get( internal2 );
    check( client_image2 && client_image2->blob && ( *client_image2->blob == pixels2 ),
           "reset identity: client ends up with exactly the second image after the diff" );
  }

  /* Scroll fallback (shortcut not taken): with every other row left blank,
     they all still share the one blank-row prototype from construction, so
     the scroll shortcut's own row-0 early-out gives up before it ever
     looks for a real scroll match (see the "wire scroll" test above, which
     prints distinct text on every row precisely to avoid this and exercise
     the shortcut-taken path instead). put_row's fallback then sees the
     placement's old row lose it and its new row gain it as two unrelated
     per-row diffs; batching removals-then-additions at the end of the
     frame must still net that out to exactly one placement, at the new
     row -- not zero, which is what a naive per-row emission produced. */
  {
    Complete server( 80, 24 );
    server.act( "\033[11;1H" ); /* row index 10, column 0; every other row blank */
    std::string pixels( 12, '\x66' );
    server.act( kitty_apc( "a=T,i=970,f=24,s=2,v=2", base64_encode( pixels ) ) );
    check( server.get_fb().get_row( 10 )->placements.size() == 1, "scroll fallback: placement created at row 10" );
    const uint32_t internal_id = server.get_fb().get_row( 10 )->placements[0]->internal_image_id;
    const uint32_t uid = server.get_fb().get_row( 10 )->placements[0]->uid;

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    Complete prev( 80, 24 );

    server.admit_image_bytes( 32768 );
    client.apply_string( server.diff_from( prev ) );
    prev = server;
    check( client.get_fb().get_row( 10 )->placements.size() == 1,
           "scroll fallback: client has the placement at row 10 before scrolling" );

    server.act( "\033[24;1H" );
    server.act( "\n" ); /* scroll the whole (blank) screen up by 1 */
    check( server.get_fb().get_row( 9 )->placements.size() == 1,
           "scroll fallback: server placement moved to row 9" );

    client.apply_string( server.diff_from( prev ) );

    int placement_rows_on_client = 0;
    for ( int r = 0; r < 24; r++ ) {
      placement_rows_on_client += static_cast<int>( client.get_fb().get_row( r )->placements.size() );
    }
    check( placement_rows_on_client == 1, "scroll fallback: exactly one placement survives on the client" );
    check( client.get_fb().get_row( 9 )->placements.size() == 1
             && client.get_fb().get_row( 9 )->placements[0]->uid == uid
             && client.get_fb().get_row( 9 )->placements[0]->internal_image_id == internal_id,
           "scroll fallback: the surviving placement is at row 9 with the server's uid" );
  }

  /* A second chunk that disagrees with the total the first chunk locked
     in is untrusted network input and gets dropped -- the pending image
     stays at its original total, and a later, correctly-sized
     continuation still completes it. */
  {
    Framebuffer fb( 80, 24 );
    auto piece1 = std::make_shared<const std::string>( std::string( 4, 'a' ) );
    fb.kitty_apply_chunk( 1000, 0, piece1, 8, 24, 1, 1, false ); /* total locked in at 8 */
    check( fb.image_store_get( 1000 ) != nullptr, "item6: first chunk creates the entry" );

    auto piece2 = std::make_shared<const std::string>( std::string( 4, 'b' ) );
    fb.kitty_apply_chunk( 1000, 4, piece2, 100, 24, 1, 1, false ); /* total disagrees: 100 != 8 */
    std::shared_ptr<const Image> still_pending = fb.image_store_get( 1000 );
    check( still_pending != nullptr && !still_pending->blob,
           "item6: a chunk with a different total than the locked-in one is dropped" );

    auto piece3 = std::make_shared<const std::string>( std::string( 4, 'c' ) );
    fb.kitty_apply_chunk( 1000, 4, piece3, 8, 24, 1, 1, false ); /* the real continuation, at the locked-in total */
    std::shared_ptr<const Image> done = fb.image_store_get( 1000 );
    check( done != nullptr && static_cast<bool>( done->blob ),
           "item6: after the disagreeing chunk is ignored, the real continuation still completes" );
    if ( done && done->blob ) {
      check( *done->blob == ( std::string( 4, 'a' ) + std::string( 4, 'c' ) ),
             "item6: the completed blob is exactly the two agreeing pieces" );
    }
  }

  /* A chunk whose payload would push received past the declared total --
     whether as the very first chunk or as a continuation -- is dropped
     rather than accepted and silently truncated or overrun at
     finalization. */
  {
    Framebuffer fb( 80, 24 );

    auto oversized_first = std::make_shared<const std::string>( std::string( 10, 'x' ) );
    fb.kitty_apply_chunk( 1001, 0, oversized_first, 5 /* smaller than the payload */, 24, 1, 1, false );
    check( fb.image_store_get( 1001 ) == nullptr,
           "item6: a first chunk overrunning its own total creates no entry" );

    auto piece1 = std::make_shared<const std::string>( std::string( 4, 'a' ) );
    fb.kitty_apply_chunk( 1002, 0, piece1, 8, 24, 1, 1, false ); /* total=8, received=4 after this */
    auto overrun = std::make_shared<const std::string>( std::string( 10, 'z' ) ); /* would push received to 14 */
    fb.kitty_apply_chunk( 1002, 4, overrun, 8, 24, 1, 1, false );
    std::shared_ptr<const Image> still_pending = fb.image_store_get( 1002 );
    check( still_pending != nullptr && !still_pending->blob,
           "item6: an overrunning continuation is dropped, image stays pending" );

    auto piece2 = std::make_shared<const std::string>( std::string( 4, 'b' ) );
    fb.kitty_apply_chunk( 1002, 4, piece2, 8, 24, 1, 1, false ); /* the real, correctly-sized continuation */
    std::shared_ptr<const Image> done = fb.image_store_get( 1002 );
    check( done != nullptr && done->blob && ( *done->blob == ( std::string( 4, 'a' ) + std::string( 4, 'b' ) ) ),
           "item6: after the overrun is ignored, the correctly-sized continuation still completes" );
  }

  /* Pending (still-assembling) images reserve their declared total toward
     the client's store cap too, not just completed images -- otherwise a
     flood of first chunks for distinct ids, each under the per-image cap
     but never completed, could reserve unbounded memory. */
  {
    Framebuffer fb( 80, 24 );
    Framebuffer::set_kitty_store_cap_for_tests( 100 );

    auto piece_a = std::make_shared<const std::string>( std::string( 10, 'a' ) );
    fb.kitty_apply_chunk( 1010, 0, piece_a, 60, 24, 1, 1, false ); /* reserves 60 of the 100-byte cap */
    check( fb.image_store_get( 1010 ) != nullptr, "item6: a first pending stub within the cap is created" );

    auto piece_b = std::make_shared<const std::string>( std::string( 10, 'b' ) );
    fb.kitty_apply_chunk( 1011, 0, piece_b, 60, 24, 1, 1, false ); /* 60 (reserved) + 60 (new) > 100 cap */
    check( fb.image_store_get( 1011 ) == nullptr,
           "item6: a second stub whose total would push the sum of reservations past the cap is refused" );

    Framebuffer::set_kitty_store_cap_for_tests( 64 * 1024 * 1024 ); /* restore the default for later tests */
  }

  /* A thousand empty chunks (offset=0, data="") for the same id must never
     append a piece: only the first chunk creates the entry, and every
     later empty chunk (offset still matches, since received never
     advances) is a legitimate no-op that queues nothing. */
  {
    Framebuffer fb( 80, 24 );
    for ( int i = 0; i < 1000; i++ ) {
      auto empty_piece = std::make_shared<const std::string>( std::string() );
      fb.kitty_apply_chunk( 1030, 0, empty_piece, 50, 24, 1, 1, false );
    }
    check( fb.image_store_get( 1030 ) != nullptr, "item7: one entry exists after a thousand empty chunks" );
    if ( fb.image_store_get( 1030 ) ) {
      check( !fb.image_store_get( 1030 )->blob, "item7: the entry is still pending (0 of 50 bytes received)" );
    }
    check( fb.kitty_pending_piece_count( 1030 ) == 0, "item7: zero pieces were appended by the empty chunks" );
  }

  /* clear_all_placements(true) (a=d,d=A) must not leak kitty_admitted or
     kitty_pending entries for the store it just cleared -- otherwise those
     maps grow, entry by entry, across every image a long session ever
     saw, and get copied into every transport snapshot along with the
     Framebuffer. A zero-valued admitted entry would be indistinguishable
     from a properly-absent one through kitty_admitted_bytes's "0 if
     unknown" default, so this admits a nonzero amount first. */
  {
    Framebuffer fb( 80, 24 );

    std::string img( 12, '\x11' );
    uint32_t internal_id = fb.image_store_put( 0, 24, 2, 2, false, std::make_shared<const std::string>( img ) );
    fb.kitty_admit_bytes( 4 );
    check( fb.kitty_admitted_bytes( internal_id ) == 4, "item8: admitted bytes recorded before clearing" );

    auto piece = std::make_shared<const std::string>( std::string( 4, 'a' ) );
    fb.kitty_apply_chunk( 5000, 0, piece, 8, 24, 1, 1, false );
    check( fb.kitty_pending_piece_count( 5000 ) == 1, "item8: pending piece queued before clearing" );

    fb.clear_all_placements( true );
    check( fb.image_store_get( internal_id ) == nullptr, "item8: image gone after clear_all_placements(true)" );
    check( fb.kitty_admitted_bytes( internal_id ) == 0,
           "item8: no stale (nonzero) admitted-bytes entry survives clear_all_placements(true)" );
    check( fb.image_store_get( 5000 ) == nullptr, "item8: pending image entry is gone too" );
    check( fb.kitty_pending_piece_count( 5000 ) == 0,
           "item8: the pending map is actually cleared, not just its Image entry" );
  }

  /* The same, through the real a=d,d=A command on a Complete: after a
     transmit and partial admission, d=A leaves no stale admitted entry. */
  {
    Complete server( 80, 24 );
    std::string pixels( 12, '\x11' );
    server.act( kitty_apc( "a=T,i=1040,f=24,s=2,v=2", base64_encode( pixels ) ) );
    const uint32_t internal_id = server.get_fb().image_store_resolve( 1040 );
    server.admit_image_bytes( 4 );
    check( server.get_fb().kitty_admitted_bytes( internal_id ) == 4,
           "item8: partial admission recorded before d=A" );

    server.act( kitty_apc( "a=d,d=A" ) );
    check( server.get_fb().image_store_resolve( 1040 ) == 0, "item8: image gone after d=A" );
    check( server.get_fb().kitty_admitted_bytes( internal_id ) == 0,
           "item8: a=d,d=A leaves no stale admitted-bytes entry for the forgotten id" );
  }

  /* d=I (forget one image by id) already routed through image_store_forget,
     which has dropped the admitted entry since the first commit on this
     branch; kept here as an explicit regression check alongside d=A's fix. */
  {
    Complete server( 80, 24 );
    std::string pixels( 12, '\x22' );
    server.act( kitty_apc( "a=T,i=1041,f=24,s=2,v=2", base64_encode( pixels ) ) );
    const uint32_t internal_id = server.get_fb().image_store_resolve( 1041 );
    server.admit_image_bytes( 4 );
    check( server.get_fb().kitty_admitted_bytes( internal_id ) == 4,
           "item8: partial admission recorded before d=I" );

    server.act( kitty_apc( "a=d,d=I,i=1041" ) );
    check( server.get_fb().image_store_resolve( 1041 ) == 0, "item8: image gone after d=I" );
    check( server.get_fb().kitty_admitted_bytes( internal_id ) == 0,
           "item8: d=I leaves no stale admitted-bytes entry for the forgotten id" );
  }

  /* admitted_image_bytes_total feeds the server's pacing window: it is the
     sum of admitted bytes over all images, grows with admit_image_bytes and
     falls back when an image is freed. */
  {
    Complete term( 80, 24 );
    const std::string pixels( 3000, 'q' );
    term.act( kitty_apc( "a=T,i=77,f=24,s=100,v=10", base64_encode( pixels ) ) );
    check( term.admitted_image_bytes_total() == 0, "admitted total starts at 0 after a=T" );
    check( term.has_unadmitted_images(), "image has unadmitted bytes after a=T" );
    term.admit_image_bytes( 1000 );
    check( term.admitted_image_bytes_total() == 1000, "admitted total follows a partial admission" );
    term.admit_image_bytes( 1 << 20 );
    check( term.admitted_image_bytes_total() == 3000, "admitted total caps at the image size" );
    check( !term.has_unadmitted_images(), "nothing left to admit once the image is fully admitted" );
    term.act( kitty_apc( "a=d,d=I,i=77" ) );
    check( term.admitted_image_bytes_total() == 0, "admitted total drops when the image is freed" );
  }

  /* Per-image in-flight accounting for the server's pacing window: a large
     image present in the acked state and then freed must not cancel out the
     bytes of a new image that the acked state has never seen (an aggregate
     subtraction would report 0 in flight and let the window overfill). */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=T,i=78,f=24,s=100,v=10", base64_encode( std::string( 3000, 'a' ) ) ) );
    term.admit_image_bytes( 1 << 20 );
    Complete acked( term ); /* the sender's last acked snapshot: image 78 fully admitted */
    check( term.image_bytes_in_flight_since( acked ) == 0, "nothing in flight right after the ack" );
    term.act( kitty_apc( "a=d,d=I,i=78" ) );
    term.act( kitty_apc( "a=T,i=79,f=24,s=100,v=50", base64_encode( std::string( 15000, 'b' ) ) ) );
    term.admit_image_bytes( 1000 );
    check( term.image_bytes_in_flight_since( acked ) == 1000,
           "a freed acked image does not offset a new image's in-flight bytes" );
    term.admit_image_bytes( 1 << 20 );
    check( term.image_bytes_in_flight_since( acked ) == 15000, "in flight grows to the new image's full size" );
    Complete acked2( term );
    check( term.image_bytes_in_flight_since( acked2 ) == 0, "and returns to 0 once that state is acked" );
  }

  if ( failures > 0 ) {
    fprintf( stderr, "%d check(s) failed\n", failures );
    return 1;
  }

  return 0;
}

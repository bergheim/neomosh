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

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "completeterminal.h"
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
    const size_t target = 8 * 1024 * 1024 + 1;
    bool first = true;
    while ( total < target ) {
      std::string controls = first ? "a=T,i=40,f=24,s=1,v=1,m=1" : "m=1";
      term.act( kitty_apc( controls, chunk_b64 ) );
      total += chunk_bytes;
      first = false;
    }
    std::string final_reply = term.act( kitty_apc( "m=0", base64_encode( std::string( 3, 'z' ) ) ) );
    check_eq( final_reply, "\033_Gi=40;EFBIG\033\\", "an image over 8 MiB replies EFBIG" );
  }

  /* The APC 8192-byte cap discards the whole command silently. */
  {
    Complete term( 80, 24 );
    std::string reply = term.act( kitty_apc( "a=T,i=50,f=24,s=1,v=1", std::string( 8200, 'A' ) ) );
    check_eq( reply, "", "an APC command over 8192 bytes is discarded silently" );
    check( term.get_fb().image_store_resolve( 50 ) == 0, "discarded APC command stored nothing" );
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
     m=0. Driven directly through Kitty::handle_apc (bypassing the 8192-byte
     per-APC-command dispatcher cap, which is a different, unrelated limit)
     so each "chunk" can carry far more than one escape sequence could. */
  {
    Framebuffer fb( 80, 24 );
    Kitty::ChunkState chunk;
    std::string one_mib_b64 = base64_encode( std::string( 1024 * 1024, 'z' ) );

    std::string reply;
    for ( int i = 0; i < 9; i++ ) {
      bool last = ( i == 8 );
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

    Framebuffer::set_kitty_store_cap_for_tests( 32 * 1024 * 1024 ); /* restore the default for later tests */
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

  /* The total placement list is capped at 1024, independent of the image
     count and byte caps -- repeated a=p must not grow it forever. */
  {
    Complete term( 80, 24 );
    term.act( kitty_apc( "a=t,i=200,f=32,s=1,v=1", base64_encode( std::string( 4, 'z' ) ) ) );
    for ( int p = 1; p <= 1024; p++ ) {
      std::string ps = std::to_string( p );
      check_eq( term.act( kitty_apc( "a=p,i=200,p=" + ps ) ),
                "\033_Gi=200,p=" + ps + ";OK\033\\",
                "placement cap: placement " + ps + " added" );
    }
    check( term.get_fb().placement_count() == 1024, "placement cap: exactly 1024 placements exist" );
    check_eq( term.act( kitty_apc( "a=p,i=200,p=1025" ) ),
              "\033_Gi=200,p=1025;ENOSPC\033\\",
              "placement cap: the 1025th placement replies ENOSPC" );
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

    Framebuffer::set_kitty_store_cap_for_tests( 32 * 1024 * 1024 ); /* restore the default for later tests */
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

  if ( failures > 0 ) {
    fprintf( stderr, "%d check(s) failed\n", failures );
    return 1;
  }

  return 0;
}

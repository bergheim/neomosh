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

/* Unit tests for the client-side Kitty graphics renderer: Display in
   GraphicsMode::KITTY, driven by a client-side Complete (kitty_ids_are_internal)
   that received a real server-generated diff. Complements terminal-kitty.cc,
   which covers the server-side emulator and WIRE-mode Display. */

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "completeterminal.h"
#include "kittygraphics.h"
#include "terminaldisplay.h"

using namespace Terminal;

static int failures = 0;

static void check( bool ok, const std::string& what )
{
  if ( !ok ) {
    fprintf( stderr, "FAILED: %s\n", what.c_str() );
    failures++;
  }
}

/* Build one Kitty APC command string: ESC _ G <controls>[;<payload_b64>] ESC \. */
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

static size_t count_occurrences( const std::string& haystack, const std::string& needle )
{
  size_t count = 0;
  size_t pos = 0;
  while ( ( pos = haystack.find( needle, pos ) ) != std::string::npos ) {
    count++;
    pos += needle.size();
  }
  return count;
}

/* Pull the local id out of the nth (0-based) "a=t,i=<id>" upload in out. */
static uint32_t extract_nth_local_id( const std::string& out, size_t n )
{
  const std::string marker = "\033_Ga=t,i=";
  size_t pos = 0;
  for ( size_t i = 0; i < n; i++ ) {
    pos = out.find( marker, pos );
    check( pos != std::string::npos, "extract_nth_local_id: fewer uploads than expected" );
    pos += marker.size();
  }
  pos = out.find( marker, pos );
  check( pos != std::string::npos, "extract_nth_local_id: fewer uploads than expected" );
  pos += marker.size();
  const size_t end = out.find( ',', pos );
  return static_cast<uint32_t>( std::stoul( out.substr( pos, end - pos ) ) );
}

/* Render one complete image+placement on a fresh Display and return the
   local id its single a=t upload used. */
static uint32_t upload_and_get_local_id( uint32_t app_id, char fill )
{
  Complete server( 80, 24 );
  const std::string pixels( 12, fill );
  server.act( kitty_apc( "a=T,i=" + std::to_string( app_id ) + ",f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
  server.admit_image_bytes( pixels.size() );
  Complete baseline( 80, 24 );
  Complete client( 80, 24 );
  client.set_kitty_ids_are_internal();
  client.apply_string( server.diff_from( baseline ) );

  Display d( false );
  d.set_graphics_mode( GraphicsMode::KITTY );
  Framebuffer blank( 80, 24 );
  const std::string out = d.new_frame( false, blank, client.get_fb() );
  return extract_nth_local_id( out, 0 );
}

int main( void )
{
  /* Base case, and the literal scenario described for the client rendering
     design: a small complete image with one placement, rendered on a
     Display that has never drawn anything (initialized == false, i.e. a
     full repaint). Output must be exactly one a=d,d=a (clearing whatever
     the real terminal already had), then exactly one a=t upload (base64
     of the blob, chunked at 4096 -- one chunk here, so it carries m=0),
     then exactly one a=p placement with C=1,q=2 and the right c/r -- in
     that order. A Display in NONE mode asked to render the very same
     frame emits no Kitty escapes at all. */
  {
    Complete server( 80, 24 );
    const std::string pixels( 12, '\x42' ); /* 2x2 RGB, format 24 */
    server.act( kitty_apc( "a=T,i=1,f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( pixels.size() );

    Complete baseline( 80, 24 );
    const std::string diff = server.diff_from( baseline );

    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( diff );

    check( client.get_fb().image_store_count() == 1, "base: client has exactly one image" );
    check( client.get_fb().get_row( 0 )->placements.size() == 1, "base: client has exactly one placement" );
    const auto placement = client.get_fb().get_row( 0 )->placements.at( 0 );
    const std::shared_ptr<const Image> image = client.get_fb().image_store_get( placement->internal_image_id );
    check( static_cast<bool>( image ) && static_cast<bool>( image->blob ), "base: image blob is complete" );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );

    const std::string out = d.new_frame( false, blank, client.get_fb() );

    check( count_occurrences( out, "\033_Ga=d,d=a" ) == 1, "base: exactly one a=d,d=a" );
    check( count_occurrences( out, "\033_Ga=t" ) == 1, "base: exactly one a=t upload" );
    check( count_occurrences( out, "\033_Ga=p" ) == 1, "base: exactly one a=p placement" );

    const size_t d_pos = out.find( "\033_Ga=d,d=a" );
    const size_t t_pos = out.find( "\033_Ga=t" );
    const size_t p_pos = out.find( "\033_Ga=p" );
    check( d_pos != std::string::npos && t_pos != std::string::npos && p_pos != std::string::npos && d_pos < t_pos
             && t_pos < p_pos,
           "base: a=d,d=a precedes the upload, which precedes the placement" );

    check( out.find( ",q=2,m=0;" + Kitty::base64_encode( pixels ) + "\033\\" ) != std::string::npos,
           "base: single upload chunk carries the base64 blob and m=0" );

    const std::string expect_cr
      = ",c=" + std::to_string( placement->columns ) + ",r=" + std::to_string( placement->rows );
    check( out.find( expect_cr ) != std::string::npos, "base: placement carries the right c/r" );
    check( out.find( ",C=1,q=2\033\\" ) != std::string::npos, "base: placement carries C=1,q=2" );

    Display none_display( false ); /* GraphicsMode::NONE is the default */
    const std::string none_out = none_display.new_frame( false, blank, client.get_fb() );
    check( none_out.find( "\033_G" ) == std::string::npos, "NONE mode: no graphics output for the same frame" );
  }

  /* A second frame with no change at all (same Framebuffer for last and
     current) outputs no Kitty escapes: nothing to upload again, nothing
     to re-place. */
  {
    Complete server( 80, 24 );
    const std::string pixels( 12, '\x11' );
    server.act( kitty_apc( "a=T,i=2,f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( pixels.size() );
    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    d.new_frame( true, blank, client.get_fb() ); /* establish; output discarded */

    const std::string out = d.new_frame( true, client.get_fb(), client.get_fb() );
    check( out.find( "\033_G" ) == std::string::npos, "no-change frame: no graphics output" );
  }

  /* A placement removed server-side (a=d,d=a) shows up on the client as
     the placement disappearing from its row; the next frame must emit
     a=d,d=i for it, and nothing else (no re-upload -- the image itself is
     still in the store, only the placement is gone). */
  {
    Complete server( 80, 24 );
    const std::string pixels( 12, '\x22' );
    server.act( kitty_apc( "a=T,i=3,f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( pixels.size() );
    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );
    const Framebuffer fb1( client.get_fb() );
    check( fb1.get_row( 0 )->placements.size() == 1, "delete: placement exists before server-side delete" );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    d.new_frame( true, blank, fb1 ); /* establish; output discarded */

    const Complete server_snapshot( server );
    server.act( kitty_apc( "a=d,d=a" ) );
    client.apply_string( server.diff_from( server_snapshot ) );
    const Framebuffer fb2( client.get_fb() );
    check( fb2.get_row( 0 )->placements.empty(), "delete: placement gone from the client framebuffer" );

    const std::string out = d.new_frame( true, fb1, fb2 );
    check( count_occurrences( out, "\033_Ga=d,d=i" ) == 1, "delete: exactly one a=d,d=i" );
    check( out.find( "\033_Ga=t" ) == std::string::npos, "delete: no re-upload" );
    check( out.find( "\033_Ga=p" ) == std::string::npos, "delete: nothing re-placed" );
  }

  /* A partially admitted image: the placement arrives before the image's
     bytes are fully admitted, so the first frame shows nothing at all; once
     the remaining bytes are admitted and the client's blob completes, the
     very next frame uploads and places it -- even though the placement's
     row never changed (the row holding it is untouched by admitting more
     image bytes: only the framebuffer's separate image store changes). */
  {
    Complete server( 80, 24 );
    const std::string pixels( 40, '\x33' ); /* format 32 (RGBA), 2x5 px = 40 bytes */
    server.act( kitty_apc( "a=T,i=4,f=32,s=2,v=5", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( 16 ); /* partial */
    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );
    const Framebuffer fb_partial( client.get_fb() );

    check( fb_partial.get_row( 0 )->placements.size() == 1, "partial: placement exists before completion" );
    const uint32_t internal_id = fb_partial.get_row( 0 )->placements.at( 0 )->internal_image_id;
    const std::shared_ptr<const Image> partial_image = fb_partial.image_store_get( internal_id );
    check( static_cast<bool>( partial_image ) && !partial_image->blob, "partial: image blob is not complete yet" );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    const std::string out1 = d.new_frame( true, blank, fb_partial );
    check( out1.find( "\033_G" ) == std::string::npos, "partial: first frame emits no graphics while incomplete" );

    const Complete server_snapshot( server );
    server.admit_image_bytes( pixels.size() - 16 ); /* the rest: now fully admitted */
    client.apply_string( server.diff_from( server_snapshot ) );
    const Framebuffer fb_complete( client.get_fb() );
    const std::shared_ptr<const Image> complete_image = fb_complete.image_store_get( internal_id );
    check( static_cast<bool>( complete_image ) && static_cast<bool>( complete_image->blob ),
           "partial: image blob is complete after admitting the rest" );

    const std::string out2 = d.new_frame( true, fb_partial, fb_complete );
    check( out2.find( "\033_Ga=t" ) != std::string::npos, "partial: second frame uploads once complete" );
    check( out2.find( "\033_Ga=p" ) != std::string::npos, "partial: second frame places once complete" );
  }

  /* A full repaint (initialized == false) after a placement is already
     uploaded and placed: a=d,d=a must precede the re-placement, the image
     must not be re-uploaded (its bytes are still valid on the real
     terminal -- d=a, not d=A), and the placement must be re-asserted even
     though `last` and `f` are the very same, unchanged Framebuffer (an
     ordinary uid-diff would otherwise see nothing "new" to place). */
  {
    Complete server( 80, 24 );
    const std::string pixels( 12, '\x44' );
    server.act( kitty_apc( "a=T,i=5,f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( pixels.size() );
    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );
    const Framebuffer fb( client.get_fb() );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    d.new_frame( true, blank, fb ); /* establish; output discarded */

    const std::string out = d.new_frame( false, fb, fb ); /* full repaint, unchanged state */
    check( count_occurrences( out, "\033_Ga=d,d=a" ) == 1, "repaint: exactly one a=d,d=a" );
    check( out.find( "\033_Ga=t" ) == std::string::npos, "repaint: no re-upload (bytes already on the terminal)" );
    check( count_occurrences( out, "\033_Ga=p" ) == 1, "repaint: the placement is re-asserted" );

    const size_t d_pos = out.find( "\033_Ga=d,d=a" );
    const size_t p_pos = out.find( "\033_Ga=p" );
    check( d_pos != std::string::npos && p_pos != std::string::npos && d_pos < p_pos,
           "repaint: a=d,d=a precedes the re-placement" );
  }

  /* reset_graphics(): turns a KITTY Display back to NONE and forgets that
     an image was ever uploaded. Re-enabling KITTY afterwards (as
     process_user_input does on a fresh OK reply) and rendering a full
     repaint of the very same state must upload again -- the old upload is
     forgotten, not assumed still good on whatever terminal is on the
     other end now. */
  {
    Complete server( 80, 24 );
    const std::string pixels( 12, '\xCC' );
    server.act( kitty_apc( "a=T,i=6,f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( pixels.size() );
    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );
    const Framebuffer fb( client.get_fb() );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    const std::string first_out = d.new_frame( false, blank, fb );
    check( count_occurrences( first_out, "\033_Ga=t" ) == 1, "reset_graphics: uploaded once before reset" );

    d.reset_graphics();
    check( d.get_graphics_mode() == GraphicsMode::NONE, "reset_graphics: graphics mode is NONE after reset" );

    d.set_graphics_mode( GraphicsMode::KITTY ); /* as process_user_input does on a fresh OK reply */
    const std::string second_out = d.new_frame( false, blank, fb ); /* the requested full repaint */
    check( count_occurrences( second_out, "\033_Ga=t" ) == 1, "reset_graphics: re-uploaded after reset" );
    check( second_out.find( "\033_Ga=p" ) != std::string::npos, "reset_graphics: re-placed after reset" );
  }

  /* Local ids: always within the reserved high range, and randomised per
     Display so two Displays don't pick the same base (skipped when
     random_device itself is unavailable in this environment -- in that
     case Display falls back to the same fixed base every time, by
     design). */
  {
    const uint32_t id_a = upload_and_get_local_id( 30, '\x01' );
    const uint32_t id_b = upload_and_get_local_id( 31, '\x02' );

    check( id_a >= KITTY_LOCAL_ID_MIN, "local id: first Display's id is in the reserved range" );
    check( id_b >= KITTY_LOCAL_ID_MIN, "local id: second Display's id is in the reserved range" );

    bool random_device_available = true;
    try {
      std::random_device rd;
      (void)rd;
    } catch ( ... ) {
      random_device_available = false;
    }
    if ( random_device_available ) {
      check( id_a != id_b, "local id: two Displays get different bases" );
    }
  }

  /* kitty_shutdown_sequence(): after uploading two images and then
     deleting only the first one's placement (server-side, image data
     kept), the sequence must still name both local ids with a=d,d=I --
     kitty's own a=d,d=A alone would miss the first image, since it no
     longer has a placement. */
  {
    Complete server( 80, 24 );
    const std::string pixels_a( 12, '\xAA' );
    server.act( kitty_apc( "a=T,i=10,f=24,s=2,v=2", Kitty::base64_encode( pixels_a ) ) );
    server.admit_image_bytes( pixels_a.size() );
    server.act( "\r\n" ); /* move down a row so the second placement lands elsewhere */
    const std::string pixels_b( 12, '\xBB' );
    server.act( kitty_apc( "a=T,i=11,f=24,s=2,v=2", Kitty::base64_encode( pixels_b ) ) );
    server.admit_image_bytes( pixels_b.size() );

    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );
    const Framebuffer fb1( client.get_fb() );
    check( fb1.get_row( 0 )->placements.size() == 1 && fb1.get_row( 1 )->placements.size() == 1,
           "shutdown: two placements before delete" );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    const std::string upload_out = d.new_frame( false, blank, fb1 );
    check( count_occurrences( upload_out, "\033_Ga=t" ) == 2, "shutdown: both images uploaded" );
    const uint32_t local_a = extract_nth_local_id( upload_out, 0 );
    const uint32_t local_b = extract_nth_local_id( upload_out, 1 );
    check( local_a != local_b, "shutdown: distinct local ids for the two images" );

    const Complete server_snapshot( server );
    server.act( kitty_apc( "a=d,d=i,i=10" ) ); /* delete only the first placement; image data stays */
    client.apply_string( server.diff_from( server_snapshot ) );
    const Framebuffer fb2( client.get_fb() );
    check( fb2.get_row( 0 )->placements.empty() && fb2.get_row( 1 )->placements.size() == 1,
           "shutdown: only the first placement is gone" );
    d.new_frame( true, fb1, fb2 ); /* render the removal; output not asserted on here */

    const std::string shutdown_seq = d.kitty_shutdown_sequence();
    check( shutdown_seq.rfind( "\033_Ga=d,d=A,q=2\033\\", 0 ) == 0, "shutdown: starts with a=d,d=A" );
    check( count_occurrences( shutdown_seq, "\033_Ga=d,d=I,i=" + std::to_string( local_a ) + ",q=2\033\\" ) == 1,
           "shutdown: names the first image (no longer placed)" );
    check( count_occurrences( shutdown_seq, "\033_Ga=d,d=I,i=" + std::to_string( local_b ) + ",q=2\033\\" ) == 1,
           "shutdown: names the second image too (still placed)" );
  }

  /* resume(): STMClient sends kitty_shutdown_sequence() before
     reset_graphics() so a suspend/resume (or roam) frees what was
     uploaded instead of orphaning it and starting a fresh id sequence.
     At the Display level: after an upload, kitty_shutdown_sequence()
     names the id; after reset_graphics(), the uploaded set is empty, so
     it no longer does (only the bare a=d,d=A remains). */
  {
    Complete server( 80, 24 );
    const std::string pixels( 12, '\xEE' );
    server.act( kitty_apc( "a=T,i=7,f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( pixels.size() );
    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );
    const Framebuffer fb( client.get_fb() );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    const std::string upload_out = d.new_frame( false, blank, fb );
    const uint32_t local_id = extract_nth_local_id( upload_out, 0 );

    const std::string before_reset = d.kitty_shutdown_sequence();
    check( before_reset.find( "\033_Ga=d,d=I,i=" + std::to_string( local_id ) + ",q=2\033\\" ) != std::string::npos,
           "resume: kitty_shutdown_sequence names the uploaded id before reset_graphics" );

    d.reset_graphics();
    const std::string after_reset = d.kitty_shutdown_sequence();
    check( after_reset == "\033_Ga=d,d=A,q=2\033\\",
           "resume: kitty_shutdown_sequence names nothing after reset_graphics (uploaded set is empty)" );
  }

  /* An image and its one placement vanishing in the same frame (a=d,d=I
     server-side deletes both together): kitty_prepare_frame already
     erases the local-id mapping and emits a=d,d=I for the image before
     put_row ever notices the placement is gone, so the queued removal
     must not allocate a fresh id just to name a placement the real
     terminal was never told about -- exactly one a=d,d=I, no a=d,d=i, and
     no leaked mapping. */
  {
    Complete server( 80, 24 );
    const std::string pixels( 12, '\xDD' );
    server.act( kitty_apc( "a=T,i=12,f=24,s=2,v=2", Kitty::base64_encode( pixels ) ) );
    server.admit_image_bytes( pixels.size() );
    Complete baseline( 80, 24 );
    Complete client( 80, 24 );
    client.set_kitty_ids_are_internal();
    client.apply_string( server.diff_from( baseline ) );
    const Framebuffer fb1( client.get_fb() );
    check( fb1.get_row( 0 )->placements.size() == 1, "vanish: placement exists before delete" );

    Display d( false );
    d.set_graphics_mode( GraphicsMode::KITTY );
    Framebuffer blank( 80, 24 );
    const std::string upload_out = d.new_frame( false, blank, fb1 );
    check( count_occurrences( upload_out, "\033_Ga=t" ) == 1, "vanish: image uploaded before delete" );
    const uint32_t local_id = extract_nth_local_id( upload_out, 0 );

    const Complete after_upload( server );
    server.act( kitty_apc( "a=d,d=I,i=12" ) ); /* removes the placement AND frees the image, together */
    client.apply_string( server.diff_from( after_upload ) );
    const Framebuffer fb2( client.get_fb() );
    check( fb2.get_row( 0 )->placements.empty(), "vanish: placement gone after delete" );

    const std::string vanish_out = d.new_frame( true, fb1, fb2 );
    check( count_occurrences( vanish_out, "\033_Ga=d,d=I,i=" + std::to_string( local_id ) + ",q=2\033\\" ) == 1,
           "vanish: exactly one a=d,d=I for the vanished image" );
    check( vanish_out.find( "\033_Ga=d,d=i" ) == std::string::npos,
           "vanish: no a=d,d=i for the already-forgotten placement" );

    /* No leaked mapping: kitty_local_ids is private, so confirmed
       indirectly -- the next image this Display ever uploads must get the
       very next sequential id after the first, not one further along
       (which is what a spurious allocation during the removal above would
       have consumed). */
    const Complete after_delete( server );
    const std::string pixels2( 12, '\xEE' );
    server.act( kitty_apc( "a=T,i=13,f=24,s=2,v=2", Kitty::base64_encode( pixels2 ) ) );
    server.admit_image_bytes( pixels2.size() );
    client.apply_string( server.diff_from( after_delete ) );
    const Framebuffer fb3( client.get_fb() );

    const std::string next_out = d.new_frame( true, fb2, fb3 );
    const uint32_t next_local_id = extract_nth_local_id( next_out, 0 );
    check( next_local_id == local_id + 1,
           "vanish: no id leaked by the removal -- the next allocation is the very next one" );
  }

  if ( failures > 0 ) {
    fprintf( stderr, "%d check(s) failed\n", failures );
    return 1;
  }

  return 0;
}

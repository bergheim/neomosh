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

/* Unit tests for terminal pixel window size (XTWINOPS CSI t) and the
   client-to-server Resize event carrying xpixel/ypixel. */

#include <cstdio>
#include <string>

#include "completeterminal.h"
#include "user.h"

using namespace Terminal;
using namespace Network;

static int failures = 0;

static void check_eq( const std::string& actual, const std::string& expected, const std::string& what )
{
  if ( actual != expected ) {
    fprintf(
      stderr, "FAILED: %s (expected %zu bytes, got %zu bytes)\n", what.c_str(), expected.size(), actual.size() );
    failures++;
  }
}

static void check( bool ok, const std::string& what )
{
  if ( !ok ) {
    fprintf( stderr, "FAILED: %s\n", what.c_str() );
    failures++;
  }
}

int main( void )
{
  /* CSI 14 t, 16 t and 18 t reply correctly at 80x24 with 800x480 pixels. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Resize( 80, 24, 800, 480 ) );
    check_eq( term.act( "\033[14t" ), "\033[4;480;800t", "CSI 14 t reports window size in pixels" );
    check_eq( term.act( "\033[16t" ), "\033[6;20;10t", "CSI 16 t reports cell size in pixels" );
    check_eq( term.act( "\033[18t" ), "\033[8;24;80t", "CSI 18 t reports text area size in characters" );
  }

  /* CSI 14 t and 16 t stay silent when pixel size is unknown (0); 18 t is unaffected. */
  {
    Complete term( 80, 24 );
    check_eq( term.act( "\033[14t" ), "", "CSI 14 t with unknown pixel size" );
    check_eq( term.act( "\033[16t" ), "", "CSI 16 t with unknown pixel size" );
    check_eq( term.act( "\033[18t" ), "\033[8;24;80t", "CSI 18 t with unknown pixel size still reports rows/cols" );
  }

  /* Only the plain single-parameter forms are answered. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Resize( 80, 24, 800, 480 ) );
    check_eq( term.act( "\033[14;2t" ), "", "CSI 14;2 t (extra parameter) produces nothing" );
    check_eq( term.act( "\033[22t" ), "", "CSI 22 t (unhandled Ps) produces nothing" );
  }

  /* A UserStream round trip preserves xpixel/ypixel on a Resize event. */
  {
    UserStream sent;
    sent.push_back( Parser::Resize( 80, 24, 800, 480 ) );

    UserStream received;
    received.apply_string( sent.diff_from( UserStream() ) );

    check( received.size() == 1, "UserStream round trip produced exactly one event" );
    const Parser::Resize& res = static_cast<const Parser::Resize&>( received.get_action( 0 ) );
    check( res == Parser::Resize( 80, 24, 800, 480 ),
           "UserStream round trip preserves width/height/xpixel/ypixel" );
  }

  /* Two Complete states differing only in pixel size compare equal. Framebuffer
     rows are compared by shared_ptr identity (copy-on-write), so start from a
     copy rather than two independently constructed terminals. */
  {
    Complete without_pixels( 80, 24 );
    Complete with_pixels( without_pixels );
    with_pixels.act( Parser::Resize( 80, 24, 800, 480 ) );

    check( with_pixels == without_pixels, "pixel-only resize does not change Complete::operator==" );
  }

  /* A resize that does not mention pixels (an emulator-internal caller) keeps the
     last known pixel size; only a Resize event from the client changes it. */
  {
    Framebuffer fb( 80, 24 );
    fb.resize( 80, 24, 800, 480 );
    fb.resize( 132, 24 );
    check( ( fb.ds.get_xpixel() == 800 ) && ( fb.ds.get_ypixel() == 480 ),
           "pixel size survives a two-argument resize" );
    fb.resize( 132, 24, 0, 0 );
    check( ( fb.ds.get_xpixel() == 0 ) && ( fb.ds.get_ypixel() == 0 ), "explicit zero pixel size clears it" );
  }

  if ( failures > 0 ) {
    fprintf( stderr, "%d check(s) failed\n", failures );
    return 1;
  }

  return 0;
}

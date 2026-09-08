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

/* Unit tests for live light/dark theme propagation (OSC 10/11 queries,
   CSI ? 996 n colour scheme query, and DEC private mode 2031 notification). */

#include <cstdio>
#include <string>

#include "completeterminal.h"

using namespace Terminal;

static int failures = 0;

static void check_eq( const std::string& actual, const std::string& expected, const std::string& what )
{
  if ( actual != expected ) {
    fprintf(
      stderr, "FAILED: %s (expected %zu bytes, got %zu bytes)\n", what.c_str(), expected.size(), actual.size() );
    failures++;
  }
}

int main( void )
{
  /* No reply when the theme is unknown. */
  {
    Complete term( 80, 24 );
    check_eq( term.act( "\033]10;?\007" ), "", "OSC 10 query with unknown theme" );
    check_eq( term.act( "\033]11;?\007" ), "", "OSC 11 query with unknown theme" );
    check_eq( term.act( "\033[?996n" ), "", "CSI ? 996 n with unknown theme" );
  }

  /* Correct replies for OSC 10, OSC 11, and CSI ? 996 n after a Theme event. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_DARK ) );
    check_eq( term.act( "\033]10;?\007" ), "\033]10;rgb:ffff/ffff/ffff\033\\", "OSC 10 query" );
    check_eq( term.act( "\033]11;?\007" ), "\033]11;rgb:0000/0000/0000\033\\", "OSC 11 query" );
    check_eq( term.act( "\033[?996n" ), "\033[?997;1n", "CSI ? 996 n after dark theme" );

    term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_LIGHT ) );
    check_eq( term.act( "\033[?996n" ), "\033[?997;2n", "CSI ? 996 n after light theme" );
  }

  /* Invalid colours never elicit a reply. */
  {
    const char* invalid_colours[] = { "zz/00/00", "00/00", "00/00/00/00", "00000/00/00" };
    for ( const char* bad : invalid_colours ) {
      Complete term( 80, 24 );
      term.act( Parser::Theme( bad, bad, Parser::Theme::SCHEME_DARK ) );
      check_eq( term.act( "\033]10;?\007" ), "", std::string( "OSC 10 query with invalid colour " ) + bad );
      check_eq( term.act( "\033]11;?\007" ), "", std::string( "OSC 11 query with invalid colour " ) + bad );
    }
  }

  /* OSC 11 set form is ignored: not stored, not answered. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_DARK ) );
    check_eq( term.act( "\033]11;#000000\007" ), "", "OSC 11 set form produces no reply" );
    check_eq( term.act( "\033]11;?\007" ),
              "\033]11;rgb:0000/0000/0000\033\\",
              "OSC 11 set form does not overwrite stored colour" );
  }

  /* DEC private mode 2031: enabled, a scheme change emits exactly one report. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_DARK ) );
    check_eq( term.act( "\033[?2031h" ), "", "enabling mode 2031 produces no reply" );
    check_eq( term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_LIGHT ) ),
              "\033[?997;2n",
              "mode 2031 notifies on dark to light change" );

    /* No emission when the scheme does not change. */
    check_eq( term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_LIGHT ) ),
              "",
              "mode 2031 does not notify when scheme is unchanged" );
  }

  /* No emission when mode 2031 is off. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_DARK ) );
    check_eq( term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_LIGHT ) ),
              "",
              "no notification when mode 2031 is off" );
  }

  /* ESC c clears mode 2031, so a later scheme change emits nothing. */
  {
    Complete term( 80, 24 );
    term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_DARK ) );
    term.act( "\033[?2031h" );
    check_eq( term.act( "\033c" ), "", "ESC c produces no reply" );
    check_eq( term.act( Parser::Theme( "ffff/ffff/ffff", "0000/0000/0000", Parser::Theme::SCHEME_LIGHT ) ),
              "",
              "ESC c cleared mode 2031, so no notification follows" );
  }

  if ( failures > 0 ) {
    fprintf( stderr, "%d check(s) failed\n", failures );
    return 1;
  }

  return 0;
}

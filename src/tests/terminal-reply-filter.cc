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

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "src/terminal/terminalreply.h"

using Terminal::TerminalReplyFilter;
typedef TerminalReplyFilter::Reply Reply;

static void fail( const std::string& test_name, const std::string& detail )
{
  fprintf( stderr, "FAIL: %s: %s\n", test_name.c_str(), detail.c_str() );
  exit( 1 );
}

static void expect( const std::string& test_name, bool cond, const std::string& detail )
{
  if ( !cond ) {
    fail( test_name, detail );
  }
}

static void expect_eq( const std::string& test_name, const std::string& what, size_t got, size_t want )
{
  if ( got != want ) {
    char buf[256];
    snprintf( buf, sizeof( buf ), "%s: got %zu, want %zu", what.c_str(), got, want );
    fail( test_name, buf );
  }
}

static void expect_eq( const std::string& test_name, const std::string& what, const std::string& got,
                       const std::string& want )
{
  if ( got != want ) {
    fail( test_name, what + ": got \"" + got + "\", want \"" + want + "\"" );
  }
}

/* Full dark 997 reply -> one COLOR_SCHEME event, empty passthrough. */
static void test_csi_dark( void )
{
  const std::string name = "csi_dark";
  TerminalReplyFilter f;
  std::string passthrough;
  f.feed( "\033[?997;1n", passthrough );

  expect_eq( name, "passthrough", passthrough, "" );
  std::vector<Reply> replies = f.take_replies();
  expect_eq( name, "reply count", replies.size(), 1 );
  expect( name, replies.at( 0 ).kind == Reply::COLOR_SCHEME, "wrong kind" );
  expect( name, replies.at( 0 ).scheme == 1, "wrong scheme" );
  expect( name, !f.has_pending(), "should have no pending bytes" );
}

/* Full light 997 reply. */
static void test_csi_light( void )
{
  const std::string name = "csi_light";
  TerminalReplyFilter f;
  std::string passthrough;
  f.feed( "\033[?997;2n", passthrough );

  expect_eq( name, "passthrough", passthrough, "" );
  std::vector<Reply> replies = f.take_replies();
  expect_eq( name, "reply count", replies.size(), 1 );
  expect( name, replies.at( 0 ).kind == Reply::COLOR_SCHEME, "wrong kind" );
  expect( name, replies.at( 0 ).scheme == 2, "wrong scheme" );
}

/* OSC 11 (background), BEL-terminated. */
static void test_osc_bg_bel( void )
{
  const std::string name = "osc_bg_bel";
  TerminalReplyFilter f;
  std::string passthrough;
  f.feed( "\033]11;rgb:1234/5678/9abc\007", passthrough );

  expect_eq( name, "passthrough", passthrough, "" );
  std::vector<Reply> replies = f.take_replies();
  expect_eq( name, "reply count", replies.size(), 1 );
  expect( name, replies.at( 0 ).kind == Reply::BACKGROUND, "wrong kind" );
  expect_eq( name, "color", replies.at( 0 ).color, "1234/5678/9abc" );
}

/* OSC 10 (foreground), ST-terminated (ESC \). */
static void test_osc_fg_st( void )
{
  const std::string name = "osc_fg_st";
  TerminalReplyFilter f;
  std::string passthrough;
  f.feed( "\033]10;rgb:aa/bb/cc\033\\", passthrough );

  expect_eq( name, "passthrough", passthrough, "" );
  std::vector<Reply> replies = f.take_replies();
  expect_eq( name, "reply count", replies.size(), 1 );
  expect( name, replies.at( 0 ).kind == Reply::FOREGROUND, "wrong kind" );
  expect_eq( name, "color", replies.at( 0 ).color, "aa/bb/cc" );
}

/* Splitting a valid reply at every byte boundary across two feed() calls
   must give identical results to feeding it all at once. */
static void test_split_at_every_boundary( void )
{
  const std::string name = "split_at_every_boundary";
  const std::vector<std::string> messages
    = { "\033[?997;1n", "\033]11;rgb:1234/5678/9abc\007", "\033]10;rgb:aa/bb/cc\033\\" };

  for ( std::vector<std::string>::const_iterator m = messages.begin(); m != messages.end(); m++ ) {
    const std::string& msg = *m;
    for ( size_t split = 0; split <= msg.size(); split++ ) {
      TerminalReplyFilter f;
      std::string passthrough;
      f.feed( msg.substr( 0, split ), passthrough );
      f.feed( msg.substr( split ), passthrough );

      expect_eq( name, "passthrough for split " + std::to_string( split ) + " of \"" + msg + "\"", passthrough, "" );
      std::vector<Reply> replies = f.take_replies();
      expect_eq( name, "reply count for split " + std::to_string( split ) + " of \"" + msg + "\"", replies.size(),
                1 );
      expect( name, !f.has_pending(), "should have no pending bytes after split " + std::to_string( split ) );
    }
  }
}

/* Keystrokes surrounding a reply pass through unchanged and in order. */
static void test_keystrokes_around_reply( void )
{
  const std::string name = "keystrokes_around_reply";
  TerminalReplyFilter f;
  std::string passthrough;
  f.feed( "ab\033[?997;2ncd", passthrough );

  expect_eq( name, "passthrough", passthrough, "abcd" );
  std::vector<Reply> replies = f.take_replies();
  expect_eq( name, "reply count", replies.size(), 1 );
  expect( name, replies.at( 0 ).kind == Reply::COLOR_SCHEME, "wrong kind" );
  expect( name, replies.at( 0 ).scheme == 2, "wrong scheme" );
}

/* A bare ESC, then unrelated bytes, must pass through untouched with no events. */
static void test_unrelated_escapes_pass_through( void )
{
  const std::vector<std::string> cases = { "\033x", "\033[A", "\033]0;title\007" };

  for ( std::vector<std::string>::const_iterator c = cases.begin(); c != cases.end(); c++ ) {
    const std::string name = "unrelated_escape[" + *c + "]";
    TerminalReplyFilter f;
    std::string passthrough;
    f.feed( *c, passthrough );

    expect_eq( name, "passthrough", passthrough, *c );
    std::vector<Reply> replies = f.take_replies();
    expect_eq( name, "reply count", replies.size(), 0 );
    expect( name, !f.has_pending(), "should have no pending bytes" );
  }
}

/* A malformed colour value passes through as bytes, unchanged. */
static void test_malformed_color( void )
{
  const std::string name = "malformed_color";
  TerminalReplyFilter f;
  std::string passthrough;
  const std::string input = "\033]10;rgb:zz/00/00\007";
  f.feed( input, passthrough );

  expect_eq( name, "passthrough", passthrough, input );
  std::vector<Reply> replies = f.take_replies();
  expect_eq( name, "reply count", replies.size(), 0 );
}

/* A held partial prefix is released by flush() unchanged. */
static void test_flush_releases_partial_prefix( void )
{
  const std::string name = "flush_releases_partial_prefix";
  TerminalReplyFilter f;
  std::string passthrough;
  const std::string input = "\033[?99";
  f.feed( input, passthrough );

  expect_eq( name, "passthrough before flush", passthrough, "" );
  expect( name, f.has_pending(), "should have pending bytes" );

  f.flush( passthrough );
  expect_eq( name, "passthrough after flush", passthrough, input );
  expect( name, !f.has_pending(), "should have no pending bytes after flush" );
  std::vector<Reply> replies = f.take_replies();
  expect_eq( name, "reply count", replies.size(), 0 );
}

/* More than 64 held bytes forces an automatic flush. */
static void test_cap_forces_flush( void )
{
  const std::string name = "cap_forces_flush";
  TerminalReplyFilter f;
  std::string passthrough;

  std::string input = "\033[?997;";
  input.append( 70, '1' ); /* never-ending digit run: no terminator seen */

  f.feed( input, passthrough );

  /* The cap must have kicked in at some point, releasing bytes to passthrough
     even though we haven't reached the (nonexistent) terminator yet. */
  expect( name, !passthrough.empty(), "expected some bytes to have been flushed by the cap" );
  expect_eq( name, "reply count", f.take_replies().size(), 0 );

  /* Nothing must be lost or reordered: draining what's held plus what has
     already leaked to passthrough must reproduce the original input. */
  std::string remaining;
  f.flush( remaining );
  expect_eq( name, "reassembled input", passthrough + remaining, input );
}

int main( void )
{
  test_csi_dark();
  test_csi_light();
  test_osc_bg_bel();
  test_osc_fg_st();
  test_split_at_every_boundary();
  test_keystrokes_around_reply();
  test_unrelated_escapes_pass_through();
  test_malformed_color();
  test_flush_releases_partial_prefix();
  test_cap_forces_flush();

  printf( "PASS\n" );
  return 0;
}

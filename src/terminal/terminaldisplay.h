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

#ifndef TERMINALDISPLAY_HPP
#define TERMINALDISPLAY_HPP

#include "src/terminal/terminalframebuffer.h"

namespace Terminal {
/* NONE: no Kitty wire emission at all (the default; also what a local
   client Display uses until the next branch's probe succeeds). WIRE: used
   by Complete's Display on the server, and by a client's Kitty module to
   interpret what WIRE emits -- placements diffed by uid, positioned and
   deleted with internal ids and q=2. KITTY: reserved for the next branch
   (a client Display talking Kitty graphics to the real local terminal);
   behaves like NONE here. */
enum class GraphicsMode
{
  NONE,
  WIRE,
  KITTY
};

/* variables used within a new_frame */
class FrameState
{
public:
  std::string str;

  int cursor_x, cursor_y;
  Renditions current_rendition;
  Hyperlink current_hyperlink;
  bool cursor_visible;

  const Framebuffer& last_frame;

  /* WIRE mode only: placement diffs collected across the *whole* frame by
     Display::put_row (by way of record_placement_diff), flushed once at
     the end of Display::new_frame -- every removal, then every addition.
     put_row's view is one row at a time, so when the scroll shortcut isn't
     taken, the same (internal id, uid) can show up as "gone" from its old
     row and "new" at its new row purely because two independent per-row
     comparisons happened to name the same placement; emitting per-row as
     each is found would let a same-uid delete meant for the old row
     clobber the add that already landed at the new one. Collecting first
     and flushing removals-before-additions nets that out to the right
     final state regardless of how many rows are involved. Empty and
     unused outside WIRE mode. */
  std::vector<std::shared_ptr<const ImagePlacement>> kitty_removals;
  std::vector<std::pair<int, std::shared_ptr<const ImagePlacement>>> kitty_additions; /* (frame_y, placement) */

  FrameState( const Framebuffer& s_last );

  void append( char c ) { str.append( 1, c ); }
  void append( size_t s, char c ) { str.append( s, c ); }
  void append( wchar_t wc ) { Cell::append_to_str( str, wc ); }
  void append( const char* s ) { str.append( s ); }
  void append_string( const std::string& append ) { str.append( append ); }

  void append_cell( const Cell& cell ) { cell.print_grapheme( str ); }
  void append_silent_move( int y, int x );
  void append_move( int y, int x );
  void update_rendition( const Renditions& r, bool force = false );
  void update_hyperlink( const Hyperlink& h, bool force = false );
};

class Display
{
private:
  bool has_ech; /* erase character is part of vt200 but not supported by tmux
                   (or by "screen" terminfo entry, which is what tmux advertises) */

  bool has_bce; /* erases result in cell filled with background color */

  bool has_title; /* supports window title and icon name */

  const char *smcup, *rmcup; /* enter and exit alternate screen mode */

  GraphicsMode graphics_mode;

  bool put_row( bool initialized,
                FrameState& frame,
                const Framebuffer& f,
                int frame_y,
                const Row& old_row,
                bool wrap ) const;

  bool can_use_erase( const FrameState& frame ) const;

  /* WIRE mode only: diff one row's placements by uid against the old row's
     and record what changed into frame's kitty_removals/kitty_additions --
     nothing is written to frame.str here. Called from put_row, once per
     row. */
  void record_placement_diff( FrameState& frame,
                              int frame_y,
                              const std::vector<std::shared_ptr<const ImagePlacement>>& old_placements,
                              const std::vector<std::shared_ptr<const ImagePlacement>>& new_placements ) const;

  /* WIRE mode only: emit every placement diff record_placement_diff
     collected across the whole frame -- all removals, then all additions
     (each with its own silent cursor move) -- and clear both lists.
     Called once, at the end of new_frame. */
  void flush_placement_diffs( FrameState& frame ) const;

public:
  std::string open() const;
  std::string close() const;

  std::string new_frame( bool initialized, const Framebuffer& last, const Framebuffer& f ) const;

  void set_graphics_mode( GraphicsMode mode ) { graphics_mode = mode; }
  GraphicsMode get_graphics_mode( void ) const { return graphics_mode; }

  Display( bool use_environment );
};
}

#endif

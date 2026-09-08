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

#include <map>
#include <set>

#include "src/terminal/terminalframebuffer.h"

namespace Terminal {
/* NONE: no Kitty wire emission at all (the default; also what a local
   client Display uses until the graphics probe succeeds, or never does).
   WIRE: used by Complete's Display on the server, and interpreted by a
   client's Kitty module -- placements diffed by uid, positioned and
   deleted with internal ids and q=2. KITTY: a client Display talking real
   Kitty graphics to the local terminal it's actually attached to, once the
   probe in stmclient.cc has confirmed it understands the protocol. */
enum class GraphicsMode
{
  NONE,
  WIRE,
  KITTY
};

/* Client-side Kitty local image id space: kept high and randomised per
   Display (see Display::Display) so it never collides with the low ids
   (1, 2, 3, ...) some other process already in the same real terminal
   plausibly picked for its own images. inline constexpr: one definition
   across every translation unit that includes this header, no separate
   .cc definition needed (C++17). */
inline constexpr uint32_t KITTY_LOCAL_ID_MIN = 0x80000000;
inline constexpr uint32_t KITTY_LOCAL_ID_MAX = 0xFF000000; /* exclusive */
inline constexpr uint32_t KITTY_LOCAL_ID_RANGE = KITTY_LOCAL_ID_MAX - KITTY_LOCAL_ID_MIN;

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

  /* WIRE and KITTY modes only: placement diffs collected across the
     *whole* frame by Display::put_row (by way of record_placement_diff),
     flushed once at the end of Display::new_frame -- every removal, then
     every addition. put_row's view is one row at a time, so when the
     scroll shortcut isn't taken, the same (internal id, uid) can show up
     as "gone" from its old row and "new" at its new row purely because two
     independent per-row comparisons happened to name the same placement;
     emitting per-row as each is found would let a same-uid delete meant
     for the old row clobber the add that already landed at the new one.
     Collecting first and flushing removals-before-additions nets that out
     to the right final state regardless of how many rows are involved.
     KITTY mode also seeds kitty_additions directly (see
     Display::kitty_prepare_frame) for placements a per-row uid-diff can
     never catch on its own -- a full repaint re-placing everything after
     a=d,d=a, or a placement whose image's blob just completed. Empty and
     unused in NONE mode. */
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

  /* KITTY mode only, mutable Display state -- not Framebuffer state, so it
     survives independently of whatever particular `last`/`f` a given
     new_frame call happens to diff, and persists across the full lifetime
     of a client session (roams, reconnects, repaints and all). */

  /* Server/session internal image id -> local id actually used with the
     real terminal, allocated as kitty_local_id_base + kitty_next_local_id
     (wrapping within [KITTY_LOCAL_ID_MIN, KITTY_LOCAL_ID_MAX)), the first
     time a placement referencing that internal id is uploaded or removed. */
  mutable std::map<uint32_t, uint32_t> kitty_local_ids;

  /* Drawn once from std::random_device when this Display is constructed
     (not on copy -- a copy inherits its source's base via the default
     copy constructor below, same as every other member here). Not
     mutable: set once at construction and never written again, so a
     const method may read it without needing write access. */
  uint32_t kitty_local_id_base;
  mutable uint32_t kitty_next_local_id; /* offset from kitty_local_id_base, wrapped mod KITTY_LOCAL_ID_RANGE */

  /* Local ids whose bytes the real terminal already has (an a=t upload was
     sent for them at some point this session). */
  mutable std::set<uint32_t> kitty_uploaded;

  /* Internal image ids present in the store as of the last frame this
     Display rendered, for delete detection: an id that drops out between
     one call and the next gets an a=d,d=I so the real terminal forgets it
     too. */
  mutable std::set<uint32_t> kitty_last_image_ids;

  bool put_row( bool initialized,
                FrameState& frame,
                const Framebuffer& f,
                int frame_y,
                const Row& old_row,
                bool wrap ) const;

  bool can_use_erase( const FrameState& frame ) const;

  /* WIRE and KITTY modes: diff one row's placements by uid against the old
     row's and record what changed into frame's kitty_removals/
     kitty_additions -- nothing is written to frame.str here. Called from
     put_row, once per row (KITTY mode skips this on a full repaint; see
     put_row). */
  void record_placement_diff( FrameState& frame,
                              int frame_y,
                              const std::vector<std::shared_ptr<const ImagePlacement>>& old_placements,
                              const std::vector<std::shared_ptr<const ImagePlacement>>& new_placements ) const;

  /* WIRE mode only: emit every placement diff record_placement_diff
     collected across the whole frame -- all removals, then all additions
     (each with its own silent cursor move) -- and clear both lists.
     Called once, at the end of new_frame. */
  void flush_placement_diffs( FrameState& frame ) const;

  /* KITTY mode only: called once at the top of new_frame, before the
     per-row loop. Emits a=d,d=I for images gone from the store since the
     last frame this Display rendered, and seeds frame.kitty_additions with
     placements an ordinary per-row uid-diff in put_row can never discover
     on its own: on a full repaint (initialized false -- a=d,d=a has just
     cleared every placement from the real terminal) every currently-live
     placement, unconditionally; otherwise, a placement whose image's blob
     just completed (present with a blob now, absent or pending before) but
     which already existed, unchanged, on its row last frame -- its uid
     never disappeared, so put_row's uid-diff would see nothing "new". A
     placement that is both new and already complete this same frame is
     deliberately left alone here; put_row's ordinary diff picks it up, so
     it is never queued twice. */
  void kitty_prepare_frame( FrameState& frame, const Framebuffer& f, bool initialized ) const;

  /* KITTY mode only: emit every placement diff kitty_prepare_frame and
     record_placement_diff collected across the whole frame, translated to
     local terminal ids -- all removals (a=d,d=i) first, then all additions
     (uploading the image first if its local id isn't in kitty_uploaded yet,
     then a silent move and a=p) -- and clear both lists. Mirrors
     flush_placement_diffs's shape and removals-before-additions rationale.
     f is the current frame's Framebuffer, needed to look up each
     placement's image and check whether its blob is complete yet. */
  void flush_kitty_placement_diffs( FrameState& frame, const Framebuffer& f ) const;

  /* KITTY mode only: look up (or, the first time, allocate from
     kitty_local_id_base/kitty_next_local_id) the local terminal id for a
     server/session internal image id. */
  uint32_t kitty_local_id_for( uint32_t internal_id ) const;

  /* KITTY mode only: look up the local terminal id for a server/session
     internal image id WITHOUT allocating one -- 0 (never a valid local id;
     they start at KITTY_LOCAL_ID_MIN > 0) if none has been. For a removal:
     allocating one here just to name a placement the real terminal was
     never told to place (its image vanished from the store, and with it
     the mapping, earlier in the very same frame -- see kitty_prepare_frame)
     would both emit a meaningless a=d,d=i and leak the fresh mapping
     forever, since nothing will ever reference that internal id again. */
  uint32_t kitty_local_id_lookup( uint32_t internal_id ) const;

  /* KITTY mode only: base64-encode image's blob and send it to the real
     terminal as local_id, in chunks of at most 4096 base64 characters
     (m=1 ... m=0). Precondition: image.blob is non-null. */
  void kitty_upload_image( FrameState& frame, uint32_t local_id, const Image& image ) const;

public:
  std::string open() const;
  std::string close() const;

  std::string new_frame( bool initialized, const Framebuffer& last, const Framebuffer& f ) const;

  void set_graphics_mode( GraphicsMode mode ) { graphics_mode = mode; }
  GraphicsMode get_graphics_mode( void ) const { return graphics_mode; }

  /* Turns graphics off and forgets every bit of per-terminal Kitty state
     (the local-id map, the uploaded set, and the last-seen image-id set)
     -- called from STMClient::resume() before re-sending the probe, since
     the terminal on the other end of the tty may now be a stranger (a
     fresh process after SIGCONT, or simply a different local terminal
     after a roam): a negative or unanswered probe then actually leaves
     graphics off instead of a stale KITTY mode running blind with ids the
     new terminal never heard of, and a positive reply's requested full
     repaint re-uploads everything from scratch, since kitty_uploaded is
     now empty, rather than assume the new terminal already has our old
     bytes. Deliberately leaves the local-id allocation counter alone: it's
     a property of this process, not of whatever terminal we're attached
     to, and continuing to advance it avoids replaying ids at a terminal
     that turns out to be the very same one. */
  void reset_graphics( void );

  /* KITTY mode: the full "forget everything" sequence to send before
     closing the display, so no image or placement outlives the session.
     Kitty's own a=d,d=A only frees images it still has a placement for;
     an uploaded image whose only placement was separately removed earlier
     in the session (a=d,d=i, or its row scrolling/resetting away) would
     otherwise survive it. So: a=d,d=A first, then an explicit a=d,d=I for
     every local id this Display ever uploaded (including ones d=A already
     freed -- redundant, but harmless under q=2). */
  std::string kitty_shutdown_sequence( void ) const;

  Display( bool use_environment );
  /* -Weffc++ wants copy control spelled out once a class has both pointer
     members (smcup/rmcup, non-owning -- they alias static terminfo
     capability strings, never freed) and container members (the KITTY
     bookkeeping above); the compiler-generated behaviour -- a shallow copy
     of the pointers, a deep copy of the containers -- is already exactly
     right, and is relied on wherever a Complete (which holds a Display) is
     copied. */
  Display( const Display& ) = default;
  Display& operator=( const Display& ) = default;
};
}

#endif

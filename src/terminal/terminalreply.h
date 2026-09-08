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

#ifndef TERMINAL_REPLY_HPP
#define TERMINAL_REPLY_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace Terminal {

/* A pure byte-stream state machine that recognises the local terminal's
   replies to our theme-discovery probe, and passes everything else
   through unchanged, in order, as ordinary keystrokes.

   Recognised replies:
     (a) ESC [ ? 997 ; N n            (N = 1 dark, 2 light)
     (b) ESC ] 10 ; rgb:R/G/B  (BEL|ST)   (foreground colour)
     (c) ESC ] 11 ; rgb:R/G/B  (BEL|ST)   (background colour)
     (d) ESC _ G <text> (ST|0x9C)        (Kitty graphics reply; ST is ESC \
                                           or the single-byte C1 form 0x9C)

   Anything that turns out not to match -- including a reply shape that
   is malformed once more bytes arrive -- is emitted to the passthrough
   string unchanged, in its original position. No I/O is performed here;
   the caller owns reading and writing bytes. */
class TerminalReplyFilter
{
public:
  struct Reply
  {
    enum Kind
    {
      COLOR_SCHEME,
      FOREGROUND,
      BACKGROUND,
      GRAPHICS
    } kind;

    std::string color; /* "R/G/B" (XParseColor form), set for FOREGROUND/BACKGROUND only */
    int scheme;        /* Theme::SCHEME_DARK or SCHEME_LIGHT, set for COLOR_SCHEME only */
    std::string text;  /* payload between "G" and the terminator, set for GRAPHICS only */

    Reply() : kind( COLOR_SCHEME ), color(), scheme( 0 ), text() {}
  };

  TerminalReplyFilter()
    : state_( GROUND ), held_(), csi_lit_idx_( 0 ), n_digits_(), osc_lit_idx_( 0 ), osc_kind_( Reply::FOREGROUND ),
      rgb_r_(), rgb_g_(), rgb_b_(), graphics_text_(), replies_()
  {}

  /* Feed more bytes read from the terminal. Recognised bytes are consumed;
     everything else (including bytes held from an earlier feed() that
     turned out not to match) is appended to passthrough, in order. */
  void feed( const std::string& bytes, std::string& passthrough );

  /* True while some bytes are held because they are a strict prefix of a
     recognised reply and we are still waiting to see whether it completes. */
  bool has_pending( void ) const { return !held_.empty(); }

  /* Give up on whatever is held and release it as ordinary keystrokes. */
  void flush( std::string& passthrough );

  /* Pull out (and clear) the replies recognised since the last call. */
  std::vector<Reply> take_replies( void );

private:
  static const size_t MAX_HELD = 256;

  enum State
  {
    GROUND,
    ESC1,
    CSI_Q,
    CSI_997,
    CSI_SEMI,
    CSI_NDIGIT,
    OSC_NUM1,
    OSC_NUM2,
    OSC_SEMI,
    OSC_RGB_LIT,
    OSC_R,
    OSC_G,
    OSC_B,
    OSC_TERM_ESC,
    APC_G,
    APC_TEXT,
    APC_TERM_ESC
  };

  State state_;
  std::string held_; /* raw bytes consumed so far that might still be a pass-through */

  size_t csi_lit_idx_;   /* progress matching the literal "997" */
  std::string n_digits_; /* digits of N in "997;Nn" */

  size_t osc_lit_idx_;  /* progress matching the literal "rgb:" */
  Reply::Kind osc_kind_; /* FOREGROUND (OSC 10) or BACKGROUND (OSC 11) */
  std::string rgb_r_, rgb_g_, rgb_b_;

  std::string graphics_text_; /* bytes accumulated between "ESC _ G" and the terminator */

  std::vector<Reply> replies_;

  void reset_accumulators( void );
  /* Byte c broke the current (partial) match: release held_ as passthrough
     and re-process c as if freshly seen at GROUND. */
  void mismatch( unsigned char c, std::string& passthrough );
  void process_byte( unsigned char c, std::string& passthrough );
  static bool is_hex_digit( unsigned char c );
};
}

#endif

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

#ifndef KITTYGRAPHICS_HPP
#define KITTYGRAPHICS_HPP

#include <cstdint>
#include <memory>
#include <string>

/* Server-side Kitty graphics protocol (https://sw.kovidgoyal.net/kitty/graphics-protocol/).
   The server never decodes pixels: it stores whatever bytes the client sent,
   plus enough metadata to place and re-serve them, and leaves rendering to
   the local terminal the client is connected to. */

namespace Terminal {
class Framebuffer;

/* An opaquely-stored image. Part of synchronised Framebuffer state. */
struct Image
{
  Image( uint32_t s_internal_id,
         uint32_t s_app_id,
         int s_format,
         int s_width,
         int s_height,
         bool s_compressed,
         std::shared_ptr<const std::string> s_blob )
    : internal_id( s_internal_id ), app_id( s_app_id ), format( s_format ), width( s_width ), height( s_height ),
      compressed( s_compressed ), blob( std::move( s_blob ) )
  {}

  uint32_t internal_id; /* immutable handle, assigned by Framebuffer::image_store_put */
  uint32_t app_id;      /* the client's `i=`; 0 means none was given */
  int format;           /* 24 = RGB, 32 = RGBA, 100 = PNG */
  int width, height;    /* pixel size */
  bool compressed;      /* o=z; stored and passed through, never decompressed */
  std::shared_ptr<const std::string> blob;
};

/* Placement metadata, stored on the anchor (top) Row of the image. */
struct ImagePlacement
{
  ImagePlacement( uint32_t s_internal_image_id,
                  uint32_t s_placement_id,
                  int s_column,
                  int s_columns,
                  int s_rows,
                  int s_z,
                  bool s_has_src_rect,
                  int s_src_x,
                  int s_src_y,
                  int s_src_w,
                  int s_src_h )
    : internal_image_id( s_internal_image_id ), placement_id( s_placement_id ), uid( 0 ), column( s_column ),
      columns( s_columns ), rows( s_rows ), z( s_z ), has_src_rect( s_has_src_rect ), src_x( s_src_x ),
      src_y( s_src_y ), src_w( s_src_w ), src_h( s_src_h )
  {}

  uint32_t internal_image_id;
  uint32_t placement_id; /* the client's `p=`; 0 means none was given */
  uint32_t uid;          /* wire-unique id, allocated by Framebuffer::add_placement; never 0 */
  int column;            /* the cursor column at placement time (anchor column) */
  int columns, rows;     /* extent in cells */
  int z;
  bool has_src_rect;
  int src_x, src_y, src_w, src_h; /* only meaningful when has_src_rect */

  bool operator==( const ImagePlacement& x ) const
  {
    return ( internal_image_id == x.internal_image_id ) && ( placement_id == x.placement_id ) && ( uid == x.uid )
           && ( column == x.column ) && ( columns == x.columns ) && ( rows == x.rows ) && ( z == x.z )
           && ( has_src_rect == x.has_src_rect ) && ( src_x == x.src_x ) && ( src_y == x.src_y )
           && ( src_w == x.src_w ) && ( src_h == x.src_h );
  }
};

namespace Kitty {
/* EFBIG cap, per image (applies to a single-shot transmit and to the fully
   assembled bytes of a chunked one alike). */
const size_t IMAGE_MAX_BYTES = 8 * 1024 * 1024;

/* Bounds on the store and the placement list, independent of the byte cap:
   a transmit or placement can be small and still be unbounded in count. */
const size_t MAX_IMAGES = 256;
const size_t MAX_PLACEMENTS = 1024;

/* Accumulator for an in-progress m=1 chunked transmission. Owned by the
   Dispatcher, outside synchronised state -- exactly like the OSC buffer, a
   partial image must never take part in Framebuffer equality. */
struct ChunkState
{
  ChunkState() : active( false ), overflow( false ), first_controls(), data(), q_override( -1 ) {}

  bool active;
  /* Set once the accumulated bytes would exceed IMAGE_MAX_BYTES. Once set,
     no further bytes are kept (data stays empty) but chunks keep draining
     silently until m=0, which then replies EFBIG without storing. */
  bool overflow;
  std::string first_controls; /* the first chunk's control string; re-parsed at finalize */
  std::string data;           /* decoded bytes accumulated so far */
  int q_override;             /* -1 = no later chunk supplied its own q */

  void reset( void )
  {
    active = false;
    overflow = false;
    first_controls.clear();
    data.clear();
    q_override = -1;
  }
};

/* Handle one complete APC payload known to start with 'G' (the leading 'G'
   is still present; this function strips it). Returns the full escape-
   sequence reply to send back to the host, or "" for no reply. */
std::string handle_apc( const std::string& payload, Framebuffer* fb, ChunkState* chunk );
}
}

#endif

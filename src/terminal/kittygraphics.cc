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

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>

#include "kittygraphics.h"
#include "terminalframebuffer.h"

using namespace Terminal;
using namespace Terminal::Kitty;

namespace {

/* A parsed Kitty control string ("key=value,key=value,..."). Values not
   given by the client keep their spec default; presence is tracked
   separately (has_*) wherever "was it given at all" matters. */
struct ParsedCommand
{
  char action = 't'; /* default action per the Kitty spec is transmit */

  bool has_i = false;
  uint32_t i = 0;
  bool has_I = false;
  uint32_t I = 0;
  bool has_p = false;
  uint32_t p = 0;

  int f = 32;   /* default format is RGBA */
  char t = 'd'; /* default transmission medium is direct */

  bool has_s = false;
  int s = 0;
  bool has_v = false;
  int v = 0;

  int m = -1; /* -1 = not given */
  char o = 0; /* 'z' = zlib deflate compressed, passed through untouched */

  bool has_q = false;
  int q = 0;

  bool has_c = false;
  int c = 0;
  bool has_r = false;
  int r = 0;

  bool has_x = false, has_y = false, has_w = false, has_h = false;
  int x = 0, y = 0, w = 0, h = 0;

  int z = 0;
  bool C = false;

  char d = 0;
};

/* Parse a decimal integer, requiring the entire string to be consumed and
   the result to fit within [lo, hi]. atoi() and strtoul() are both unsafe
   here: atoi() on an out-of-range string is undefined behaviour, and a
   strtoul()-plus-uint32_t-cast silently truncates instead of rejecting.
   This is the one checked helper every numeric key goes through. */
bool parse_checked( const std::string& val, long long lo, long long hi, long long& out )
{
  if ( val.empty() ) {
    return false;
  }
  errno = 0;
  char* end = NULL;
  long long v = strtoll( val.c_str(), &end, 10 );
  if ( ( errno == ERANGE ) || ( end != val.c_str() + val.size() ) ) {
    return false;
  }
  if ( ( v < lo ) || ( v > hi ) ) {
    return false;
  }
  out = v;
  return true;
}

/* Parse an int-typed key: any value that fits in int. */
bool parse_checked_int( const std::string& val, int& out )
{
  long long v;
  if ( !parse_checked( val, INT_MIN, INT_MAX, v ) ) {
    return false;
  }
  out = static_cast<int>( v );
  return true;
}

/* Parse a Kitty id key (i, I or p): [lo, 4294967295], lo=1 for i/I (id 0 is
   not a valid id), lo=0 for p (0 means "no placement id", a valid value). */
bool parse_checked_id( const std::string& val, long long lo, uint32_t& out )
{
  long long v;
  if ( !parse_checked( val, lo, 4294967295LL, v ) ) {
    return false;
  }
  out = static_cast<uint32_t>( v );
  return true;
}

/* Parse one control string ("key=value,key=value,..."). Returns false if
   any recognised key's value failed to parse (out of range, non-numeric
   trailing garbage, etc.); has_* for that key is left false in that case,
   so a caller that ignores the return value (a continuation piece, where a
   bad value is simply ignored rather than aborting the whole transmission)
   still gets sane defaults. Unrecognised keys are always ignored. */
bool parse_controls( const std::string& s, ParsedCommand& cmd )
{
  bool ok = true;
  size_t pos = 0;
  while ( pos <= s.size() ) {
    size_t comma = s.find( ',', pos );
    std::string piece = s.substr( pos, comma == std::string::npos ? std::string::npos : comma - pos );

    if ( !piece.empty() ) {
      size_t eq = piece.find( '=' );
      if ( eq == 1 ) { /* every recognised key is a single character */
        char key = piece[0];
        std::string val = piece.substr( eq + 1 );
        switch ( key ) {
          case 'a':
            if ( !val.empty() )
              cmd.action = val[0];
            break;
          case 'i':
            if ( parse_checked_id( val, 1, cmd.i ) )
              cmd.has_i = true;
            else
              ok = false;
            break;
          case 'I':
            if ( parse_checked_id( val, 1, cmd.I ) )
              cmd.has_I = true;
            else
              ok = false;
            break;
          case 'p':
            if ( parse_checked_id( val, 0, cmd.p ) )
              cmd.has_p = true;
            else
              ok = false;
            break;
          case 'f':
            if ( !parse_checked_int( val, cmd.f ) )
              ok = false;
            break;
          case 't':
            if ( !val.empty() )
              cmd.t = val[0];
            break;
          case 's':
            if ( parse_checked_int( val, cmd.s ) )
              cmd.has_s = true;
            else
              ok = false;
            break;
          case 'v':
            if ( parse_checked_int( val, cmd.v ) )
              cmd.has_v = true;
            else
              ok = false;
            break;
          case 'S': /* byte size hint; unused, the actual decoded size is authoritative */
          case 'O': /* file offset hint; unused, only t=d is supported */
            break;
          case 'm':
            if ( !parse_checked_int( val, cmd.m ) )
              ok = false;
            break;
          case 'o':
            if ( !val.empty() )
              cmd.o = val[0];
            break;
          case 'q':
            if ( parse_checked_int( val, cmd.q ) )
              cmd.has_q = true;
            else
              ok = false;
            break;
          case 'c':
            if ( parse_checked_int( val, cmd.c ) )
              cmd.has_c = true;
            else
              ok = false;
            break;
          case 'r':
            if ( parse_checked_int( val, cmd.r ) )
              cmd.has_r = true;
            else
              ok = false;
            break;
          case 'x':
            if ( parse_checked_int( val, cmd.x ) )
              cmd.has_x = true;
            else
              ok = false;
            break;
          case 'y':
            if ( parse_checked_int( val, cmd.y ) )
              cmd.has_y = true;
            else
              ok = false;
            break;
          case 'w':
            if ( parse_checked_int( val, cmd.w ) )
              cmd.has_w = true;
            else
              ok = false;
            break;
          case 'h':
            if ( parse_checked_int( val, cmd.h ) )
              cmd.has_h = true;
            else
              ok = false;
            break;
          case 'z':
            if ( !parse_checked_int( val, cmd.z ) )
              ok = false;
            break;
          case 'C': {
            int c_val;
            if ( parse_checked_int( val, c_val ) )
              cmd.C = ( c_val != 0 );
            else
              ok = false;
            break;
          }
          case 'd':
            if ( !val.empty() )
              cmd.d = val[0];
            break;
          default:
            break; /* unknown keys are ignored */
        }
      }
    }

    if ( comma == std::string::npos ) {
      break;
    }
    pos = comma + 1;
  }
  return ok;
}

/* Reject a command outright (EINVAL) if a size key -- c, r, s or v -- is out
   of a sane range: these directly drive integer arithmetic (cursor column
   plus extent, aspect ratio math) that must not be handed something like
   c=2147483647. The positional/cosmetic keys -- x, y, w, h, z -- are merely
   clamped in place instead, since an out-of-range value there is harmless
   once bounded. Called once per executed command, right after parsing. */
bool clamp_and_validate_ranges( ParsedCommand& cmd )
{
  auto in_range = []( int v, int lo, int hi ) { return ( v >= lo ) && ( v <= hi ); };

  if ( cmd.has_c && !in_range( cmd.c, 1, 10000 ) ) {
    return false;
  }
  if ( cmd.has_r && !in_range( cmd.r, 1, 10000 ) ) {
    return false;
  }
  if ( cmd.has_s && !in_range( cmd.s, 1, 10000 ) ) {
    return false;
  }
  if ( cmd.has_v && !in_range( cmd.v, 1, 10000 ) ) {
    return false;
  }

  auto clamp = []( int v, int lo, int hi ) { return v < lo ? lo : ( v > hi ? hi : v ); };
  if ( cmd.has_x ) {
    cmd.x = clamp( cmd.x, 0, 1000000 );
  }
  if ( cmd.has_y ) {
    cmd.y = clamp( cmd.y, 0, 1000000 );
  }
  if ( cmd.has_w ) {
    cmd.w = clamp( cmd.w, 0, 1000000 );
  }
  if ( cmd.has_h ) {
    cmd.h = clamp( cmd.h, 0, 1000000 );
  }
  cmd.z = clamp( cmd.z, -1000000, 1000000 );

  return true;
}

/* Clamp an aspect-ratio-derived extent (computed in double, since the
   source image's pixel size is untrusted client-controlled input up to
   ~2^31) to a range that always fits safely in an int before the cast. */
int clamp_extent_double( double v )
{
  if ( !( v >= 1.0 ) ) { /* also catches NaN */
    return 1;
  }
  if ( v > 10000.0 ) {
    return 10000;
  }
  return static_cast<int>( v );
}

/* A bare continuation chunk (once m=1 has started a transmission) carries
   only m and q. */
bool is_pure_mq_controls( const std::string& s )
{
  size_t pos = 0;
  while ( pos <= s.size() ) {
    size_t comma = s.find( ',', pos );
    std::string piece = s.substr( pos, comma == std::string::npos ? std::string::npos : comma - pos );
    if ( !piece.empty() ) {
      if ( piece[0] != 'm' && piece[0] != 'q' ) {
        return false;
      }
    }
    if ( comma == std::string::npos ) {
      break;
    }
    pos = comma + 1;
  }
  return true;
}

/* A chunk continues the first chunk's transmission if it carries only m/q
   keys, or if it repeats (as real clients do) the original a=/i=/I= and
   they agree with the first chunk's. Only a genuinely different command --
   a different action, or a different i/I -- interrupts the accumulation. */
bool is_continuation( const ParsedCommand& first, const std::string& controls_str )
{
  if ( is_pure_mq_controls( controls_str ) ) {
    return true;
  }

  ParsedCommand piece;
  parse_controls( controls_str, piece );

  if ( piece.action != first.action ) {
    return false;
  }
  if ( piece.has_i && ( !first.has_i || piece.i != first.i ) ) {
    return false;
  }
  if ( piece.has_I && ( !first.has_I || piece.I != first.I ) ) {
    return false;
  }
  return true;
}

int base64_value( char c )
{
  if ( c >= 'A' && c <= 'Z' )
    return c - 'A';
  if ( c >= 'a' && c <= 'z' )
    return c - 'a' + 26;
  if ( c >= '0' && c <= '9' )
    return c - '0' + 52;
  if ( c == '+' )
    return 62;
  if ( c == '/' )
    return 63;
  return -1;
}

/* Small, self-contained base64 decoder. Written here rather than linking
   src/crypto's base64 from the terminal library: pulling crypto into
   libmoshterminal changes the archive link order in ways that break the
   test binaries. */
std::string base64_decode( const std::string& in )
{
  std::string out;
  out.reserve( in.size() / 4 * 3 + 3 );
  /* val must be unsigned: the low bits already consumed by a previous
     extraction are never masked off, only shifted further left, so a
     signed accumulator would eventually shift a set bit into the sign bit
     -- undefined behaviour -- on long input. Unsigned left shift and
     overflow are both well defined, and the extraction below only ever
     reads a small, always-in-range window near the bottom of val, so the
     unconsumed high bits silently wrapping around changes nothing observable. */
  unsigned int val = 0;
  int bits = -8;
  for ( unsigned char uc : in ) {
    if ( uc == '=' ) {
      break;
    }
    int v = base64_value( static_cast<char>( uc ) );
    if ( v < 0 ) {
      continue; /* skip anything not part of the alphabet */
    }
    val = ( val << 6 ) + v;
    bits += 6;
    if ( bits >= 0 ) {
      out.push_back( static_cast<char>( ( val >> bits ) & 0xFF ) );
      bits -= 8;
    }
  }
  return out;
}

} // namespace

/* Small, self-contained base64 encoder, for the same reason base64_decode
   above is: pulling src/crypto's implementation into libmoshterminal
   changes the archive link order in ways that break the test binaries.
   External linkage (unlike base64_decode): the client Display uploads
   image bytes through this, from terminaldisplay.cc. */
std::string Terminal::Kitty::base64_encode( const std::string& in )
{
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve( ( in.size() + 2 ) / 3 * 4 );
  size_t i = 0;
  while ( i + 3 <= in.size() ) {
    unsigned int n = ( static_cast<unsigned char>( in[i] ) << 16 )
                     | ( static_cast<unsigned char>( in[i + 1] ) << 8 ) | static_cast<unsigned char>( in[i + 2] );
    out.push_back( table[( n >> 18 ) & 0x3F] );
    out.push_back( table[( n >> 12 ) & 0x3F] );
    out.push_back( table[( n >> 6 ) & 0x3F] );
    out.push_back( table[n & 0x3F] );
    i += 3;
  }
  const size_t rem = in.size() - i;
  if ( rem == 1 ) {
    unsigned int n = static_cast<unsigned char>( in[i] ) << 16;
    out.push_back( table[( n >> 18 ) & 0x3F] );
    out.push_back( table[( n >> 12 ) & 0x3F] );
    out.append( "==" );
  } else if ( rem == 2 ) {
    unsigned int n
      = ( static_cast<unsigned char>( in[i] ) << 16 ) | ( static_cast<unsigned char>( in[i + 1] ) << 8 );
    out.push_back( table[( n >> 18 ) & 0x3F] );
    out.push_back( table[( n >> 12 ) & 0x3F] );
    out.push_back( table[( n >> 6 ) & 0x3F] );
    out.push_back( '=' );
  }
  return out;
}

namespace {

uint32_t read_be32( const std::string& data, size_t offset )
{
  return ( static_cast<uint32_t>( static_cast<unsigned char>( data[offset] ) ) << 24 )
         | ( static_cast<uint32_t>( static_cast<unsigned char>( data[offset + 1] ) ) << 16 )
         | ( static_cast<uint32_t>( static_cast<unsigned char>( data[offset + 2] ) ) << 8 )
         | static_cast<uint32_t>( static_cast<unsigned char>( data[offset + 3] ) );
}

/* Validate the PNG signature and IHDR tag, and read width/height from bytes
   16..23. The rest of the file is never inspected: the server does not
   decode pixels. */
bool read_png_dims( const std::string& data, int& width, int& height )
{
  static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  if ( data.size() < 24 ) {
    return false;
  }
  for ( int i = 0; i < 8; i++ ) {
    if ( static_cast<unsigned char>( data[i] ) != sig[i] ) {
      return false;
    }
  }
  if ( !( data[12] == 'I' && data[13] == 'H' && data[14] == 'D' && data[15] == 'R' ) ) {
    return false;
  }
  uint32_t w = read_be32( data, 16 );
  uint32_t h = read_be32( data, 20 );
  if ( w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu ) {
    return false;
  }
  width = static_cast<int>( w );
  height = static_cast<int>( h );
  return true;
}

/* Build the reply escape sequence, applying the id-presence and q gating
   rules. Returns "" for no reply. */
std::string build_reply( const ParsedCommand& cmd, const std::string& message )
{
  if ( !cmd.has_i && !cmd.has_I ) {
    return "";
  }
  bool ok = ( message == "OK" );
  if ( cmd.q >= 2 ) {
    return "";
  }
  if ( cmd.q == 1 && ok ) {
    return "";
  }

  std::string reply = "\033_G";
  bool first = true;
  auto add = [&]( const std::string& kv ) {
    if ( !first ) {
      reply.push_back( ',' );
    }
    reply += kv;
    first = false;
  };
  if ( cmd.has_i ) {
    add( "i=" + std::to_string( cmd.i ) );
  }
  if ( cmd.has_I ) {
    add( "I=" + std::to_string( cmd.I ) );
  }
  if ( cmd.has_p ) {
    add( "p=" + std::to_string( cmd.p ) );
  }
  reply.push_back( ';' );
  reply += message;
  reply += "\033\\";
  return reply;
}

/* Extent and scroll-as-if-text placement, shared by a=T (fresh transmit) and
   a=p (place an already-stored image). See the design note's "Server
   emulator" section for the derivation of the scroll and anchor math.
   Returns false (nothing changed -- caller replies ENOSPC) if adding this
   placement would exceed the placement cap; a placement that replaces an
   existing same-id one (see below) never counts against that cap. */
bool place_image( Framebuffer* fb, uint32_t internal_id, const Image& image, const ParsedCommand& cmd )
{
  const int cell_w = fb->ds.get_cell_width_px();
  const int cell_h = fb->ds.get_cell_height_px();
  const int img_w = image.width;
  const int img_h = image.height;

  /* c and r, when given, are already range-validated (clamp_and_validate_ranges
     rejects the command outright otherwise), so they need no further clamping
     here. Derived dimensions come from untrusted image pixel sizes (PNG IHDR
     can claim up to ~2^31), so every ceil()-and-cast is clamped in double
     before the cast to keep it representable and sane. */
  int cols, rows;
  if ( cmd.has_c && cmd.has_r ) {
    cols = cmd.c;
    rows = cmd.r;
  } else if ( cmd.has_c ) {
    cols = cmd.c;
    if ( cell_w > 0 && cell_h > 0 && img_w > 0 ) {
      double disp_w = static_cast<double>( cols ) * cell_w;
      double disp_h = disp_w * img_h / img_w;
      rows = clamp_extent_double( std::ceil( disp_h / cell_h ) );
    } else {
      rows = cols; /* no pixel geometry to derive an aspect ratio from */
    }
  } else if ( cmd.has_r ) {
    rows = cmd.r;
    if ( cell_w > 0 && cell_h > 0 && img_h > 0 ) {
      double disp_h = static_cast<double>( rows ) * cell_h;
      double disp_w = disp_h * img_w / img_h;
      cols = clamp_extent_double( std::ceil( disp_w / cell_w ) );
    } else {
      cols = rows;
    }
  } else {
    if ( cell_w > 0 && cell_h > 0 ) {
      cols = clamp_extent_double( std::ceil( static_cast<double>( img_w ) / cell_w ) );
      rows = clamp_extent_double( std::ceil( static_cast<double>( img_h ) / cell_h ) );
    } else {
      cols = 1;
      rows = 1;
    }
  }

  const int width = fb->ds.get_width();
  const int orig_row = fb->ds.get_cursor_row();
  const int orig_col = fb->ds.get_cursor_col();

  /* A nonzero placement id that already exists for this image is replaced,
     not added to: find and drop it first (from wherever it currently is)
     so the placement-count check below sees the post-replacement count. */
  const bool is_replace = cmd.has_p && ( cmd.p != 0 ) && fb->placement_exists( internal_id, cmd.p );
  const size_t projected_count = fb->placement_count() + ( is_replace ? 0 : 1 );
  if ( projected_count > Kitty::MAX_PLACEMENTS ) {
    return false;
  }
  if ( is_replace ) {
    fb->delete_placement_by_id( internal_id, cmd.p );
  }

  int anchor_row;
  if ( cmd.C ) {
    /* C=1: no scrolling, no cursor movement at all. Anchor exactly at the
       cursor's current row and keep the full computed row count even if it
       runs past the bottom of the screen -- the renderer clips it. */
    anchor_row = orig_row;
  } else {
    /* rows is the full computed extent and is what gets stored on the
       placement below -- renderers clip it. Only the scroll amount is
       clamped to the screen height, since scrolling further than that is
       meaningless. */
    const int height = fb->ds.get_height();
    const int scroll_rows = ( rows > height ) ? height : rows;
    /* Advancing scroll_rows-1 rows with the framebuffer's normal autoscroll
       is exactly "scroll like text would": a no-op if the image already
       fits below the cursor, and otherwise scrolls (clamped to the screen,
       by construction, since scroll_rows is already clamped to height). */
    fb->move_rows_autoscroll( scroll_rows - 1 );
    const int bottom_row = fb->ds.get_cursor_row();
    anchor_row = bottom_row - ( scroll_rows - 1 );
    if ( anchor_row < 0 ) {
      anchor_row = 0;
    }
  }

  const bool has_src_rect = cmd.has_x || cmd.has_y || cmd.has_w || cmd.has_h;
  auto placement = std::make_shared<ImagePlacement>(
    internal_id, cmd.has_p ? cmd.p : 0, orig_col, cols, rows, cmd.z, has_src_rect, cmd.x, cmd.y, cmd.w, cmd.h );

  /* On the client (kitty_ids_are_internal), the server already chose this
     placement's wire-unique id and sent it as p=; use it verbatim instead
     of minting a new one, so a later a=d,d=i,p= from the server (which
     targets that same value) finds it. */
  const uint32_t forced_uid = ( fb->get_kitty_ids_are_internal() && cmd.has_p ) ? cmd.p : 0;
  fb->add_placement( anchor_row, placement, forced_uid );

  if ( !cmd.C ) {
    int final_col = orig_col + cols;
    if ( final_col > width - 1 ) {
      final_col = width - 1;
    }
    fb->ds.move_col( final_col );
    /* row is already at bottom_row, left there by move_rows_autoscroll */
  }
  /* C=1: cursor stays exactly where it was; nothing left to do. */

  return true;
}

/* Resolve the internal id an a=p or a=d,d=i/d=I command targets: by app id
   (i=) if given, else by image number (I=) through the number map. On the
   client (kitty_ids_are_internal), i= already *is* the internal id -- the
   server put it there directly, there is no app-id map to go through. */
uint32_t resolve_target( const ParsedCommand& cmd, Framebuffer* fb )
{
  if ( fb->get_kitty_ids_are_internal() ) {
    return cmd.has_i ? cmd.i : 0;
  }
  if ( cmd.has_i ) {
    return fb->image_store_resolve( cmd.i );
  }
  if ( cmd.has_I ) {
    return fb->image_store_resolve_number( cmd.I );
  }
  return 0;
}

/* Shared by a=q (validate only), a=t (store only) and a=T (store + place).
   cmd is taken by value: a transmit with I= and no i= allocates an app id
   and folds it into cmd.i/cmd.has_i so build_reply echoes it. */
std::string do_transmit( ParsedCommand cmd, std::string&& data, Framebuffer* fb, bool store, bool place )
{
  if ( cmd.t != 'd' ) {
    return build_reply( cmd, "EBADF:unsupported transmission medium" );
  }

  int width = 0, height = 0;
  if ( cmd.f == 100 ) {
    if ( !read_png_dims( data, width, height ) ) {
      return build_reply( cmd, "EBADPNG" );
    }
  } else if ( cmd.f == 24 || cmd.f == 32 ) {
    if ( !cmd.has_s || !cmd.has_v || cmd.s <= 0 || cmd.v <= 0 ) {
      return build_reply( cmd, "EINVAL:missing width or height" );
    }
    width = cmd.s;
    height = cmd.v;
  } else {
    return build_reply( cmd, "EINVAL:unsupported format" );
  }

  if ( data.size() > IMAGE_MAX_BYTES ) {
    return build_reply( cmd, "EFBIG" );
  }

  if ( !store ) {
    return build_reply( cmd, "OK" ); /* a=q: validated, nothing stored */
  }

  /* An empty payload, or one whose size doesn't match its declared pixel
     dimensions, would otherwise let a remote app mint an arbitrary number
     of cheap "images" (a=t,f=24,s=1,v=1 with no payload costs nothing but
     a map entry). Only applies to an actual store: a=q already returned
     above, and o=z payloads are compressed, so their decoded size is not
     known without decompressing, which the server never does. */
  if ( data.empty() ) {
    return build_reply( cmd, "EINVAL:empty payload" );
  }
  if ( cmd.o != 'z' && ( cmd.f == 24 || cmd.f == 32 ) ) {
    const size_t bytes_per_pixel = ( cmd.f == 24 ) ? 3 : 4;
    const size_t expected = static_cast<size_t>( width ) * static_cast<size_t>( height ) * bytes_per_pixel;
    if ( data.size() != expected ) {
      return build_reply( cmd, "EINVAL:payload size does not match dimensions" );
    }
  }

  /* I= with no i=: allocate a server-side app id, so this transmit is
     addressable exactly like one that gave i= explicitly, and fold it into
     cmd so the reply and the placement below see it. */
  if ( !cmd.has_i && cmd.has_I ) {
    uint32_t allocated = fb->kitty_allocate_app_id_for_number();
    if ( allocated == 0 ) {
      return build_reply( cmd, "ENOSPC" ); /* every server-allocated id is already claimed */
    }
    cmd.i = allocated;
    cmd.has_i = true;
  }
  const uint32_t app_id = cmd.has_i ? cmd.i : 0;

  if ( !fb->image_store_make_room( app_id, data.size() ) ) {
    return build_reply( cmd, "ENOSPC" );
  }

  uint32_t internal_id = fb->image_store_put(
    app_id, cmd.f, width, height, cmd.o == 'z', std::make_shared<const std::string>( std::move( data ) ) );

  if ( cmd.has_I ) {
    fb->image_store_set_number( cmd.I, internal_id ); /* latest wins */
  }

  if ( place ) {
    auto image = fb->image_store_get( internal_id );
    if ( !place_image( fb, internal_id, *image, cmd ) ) {
      return build_reply( cmd, "ENOSPC" );
    }
  }

  return build_reply( cmd, "OK" );
}

std::string do_place_existing( const ParsedCommand& cmd, Framebuffer* fb )
{
  uint32_t internal_id = resolve_target( cmd, fb );
  if ( internal_id == 0 ) {
    return build_reply( cmd, "ENOENT" );
  }
  auto image = fb->image_store_get( internal_id );
  if ( !image ) {
    return build_reply( cmd, "ENOENT" );
  }
  if ( !place_image( fb, internal_id, *image, cmd ) ) {
    return build_reply( cmd, "ENOSPC" );
  }
  return build_reply( cmd, "OK" );
}

std::string do_delete( const ParsedCommand& cmd, Framebuffer* fb )
{
  switch ( cmd.d ) {
    case 0: /* a bare a=d with no d= key defaults to d=a, per the Kitty spec */
    case 'a':
      fb->clear_all_placements( false );
      return build_reply( cmd, "OK" );
    case 'A':
      fb->clear_all_placements( true );
      return build_reply( cmd, "OK" );
    case 'i':
    case 'I': {
      uint32_t internal_id = resolve_target( cmd, fb );
      if ( internal_id == 0 ) {
        return build_reply( cmd, "ENOENT" );
      }
      if ( cmd.has_p ) {
        fb->delete_placement_by_id( internal_id, cmd.p );
      } else {
        fb->delete_placements_of_image( internal_id );
      }
      /* Free the image's data only once nothing still places it: other
         placements of this image may remain (e.g. a delete scoped to one
         p=), and freeing data out from under them would leave a dangling
         reference. */
      if ( ( cmd.d == 'I' ) && !fb->image_has_placement( internal_id ) ) {
        fb->image_store_forget( internal_id );
      }
      return build_reply( cmd, "OK" );
    }
    default:
      return ""; /* other d values are ignored */
  }
}

std::string execute_command( ParsedCommand cmd, std::string&& data, Framebuffer* fb )
{
  if ( !clamp_and_validate_ranges( cmd ) ) {
    return build_reply( cmd, "EINVAL:parameter out of range" );
  }

  switch ( cmd.action ) {
    case 'q':
      return do_transmit( cmd, std::move( data ), fb, /* store = */ false, /* place = */ false );
    case 'T':
      /* The client never receives pixels through APC: image bytes arrive as
         ImageChunk instructions instead, applied directly to the store. */
      if ( fb->get_kitty_ids_are_internal() ) {
        return build_reply( cmd, "EPERM:transmit not allowed" );
      }
      return do_transmit( cmd, std::move( data ), fb, /* store = */ true, /* place = */ true );
    case 't':
      if ( fb->get_kitty_ids_are_internal() ) {
        return build_reply( cmd, "EPERM:transmit not allowed" );
      }
      return do_transmit( cmd, std::move( data ), fb, /* store = */ true, /* place = */ false );
    case 'p':
      return do_place_existing( cmd, fb );
    case 'd':
      return do_delete( cmd, fb );
    default:
      return build_reply( cmd, "EINVAL:unsupported action" );
  }
}

/* Append decoded bytes to the chunk accumulator, respecting IMAGE_MAX_BYTES.
   Once the accumulator has overflowed, further bytes are dropped (not even
   decoded-and-discarded -- callers skip decoding once overflow is set) so a
   remote app cannot grow chunk->data without bound just by not sending m=0. */
void accumulate_chunk_bytes( ChunkState* chunk, std::string&& decoded )
{
  if ( chunk->data.size() + decoded.size() > IMAGE_MAX_BYTES ) {
    chunk->overflow = true;
    chunk->data.clear();
    return;
  }
  chunk->data.append( decoded );
}

} // namespace

std::string Terminal::Kitty::handle_apc( const std::string& payload, Framebuffer* fb, ChunkState* chunk )
{
  /* payload[0] == 'G', already checked by the caller (Dispatcher::APC_dispatch) */
  std::string body = payload.substr( 1 );
  size_t semi = body.find( ';' );
  std::string controls_str = ( semi == std::string::npos ) ? body : body.substr( 0, semi );
  std::string payload_b64 = ( semi == std::string::npos ) ? std::string() : body.substr( semi + 1 );

  if ( chunk->active ) {
    ParsedCommand first_cmd;
    parse_controls( chunk->first_controls, first_cmd );

    if ( !is_continuation( first_cmd, controls_str ) ) {
      /* Interrupted: reply for the aborted transmission (respecting its own
         q), then process the interrupting command fresh, as if it had
         arrived with no chunk in progress, and concatenate both replies. */
      if ( chunk->q_override != -1 ) {
        first_cmd.has_q = true;
        first_cmd.q = chunk->q_override;
      }
      chunk->reset();
      std::string abort_reply = build_reply( first_cmd, "EINVAL:chunked transmission interrupted" );
      std::string resumed_reply = handle_apc( payload, fb, chunk );
      return abort_reply + resumed_reply;
    }

    ParsedCommand piece;
    parse_controls( controls_str, piece );

    if ( !chunk->overflow ) {
      accumulate_chunk_bytes( chunk, base64_decode( payload_b64 ) );
    }
    /* already over the cap: keep draining silently, without decoding or
       growing memory for bytes we are just going to discard */

    if ( piece.has_q ) {
      chunk->q_override = piece.q;
    }
    if ( piece.m == 1 ) {
      return ""; /* keep accumulating */
    }

    ParsedCommand final_cmd = first_cmd;
    if ( chunk->q_override != -1 ) {
      final_cmd.has_q = true;
      final_cmd.q = chunk->q_override;
    }
    bool overflowed = chunk->overflow;
    std::string full_data = std::move( chunk->data );
    chunk->reset();

    if ( overflowed ) {
      return build_reply( final_cmd, "EFBIG" );
    }
    return execute_command( final_cmd, std::move( full_data ), fb );
  }

  ParsedCommand cmd;
  if ( !parse_controls( controls_str, cmd ) ) {
    return build_reply( cmd, "EINVAL:invalid parameter" );
  }

  if ( cmd.m == 1 ) {
    chunk->active = true;
    chunk->overflow = false;
    chunk->first_controls = controls_str;
    chunk->data.clear();
    accumulate_chunk_bytes( chunk, base64_decode( payload_b64 ) );
    chunk->q_override = -1;
    return "";
  }

  return execute_command( cmd, base64_decode( payload_b64 ), fb );
}

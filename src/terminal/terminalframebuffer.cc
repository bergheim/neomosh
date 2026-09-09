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

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "src/terminal/terminalframebuffer.h"

using namespace Terminal;

Cell::Cell( color_type background_color )
  : contents(), renditions( background_color ), hyperlink(), wide( false ), fallback( false ), wrap( false )
{}

void Cell::reset( color_type background_color )
{
  contents.clear();
  renditions = Renditions( background_color );
  hyperlink = Hyperlink();
  wide = false;
  fallback = false;
  wrap = false;
}

void DrawState::reinitialize_tabs( unsigned int start )
{
  assert( default_tabs );
  for ( unsigned int i = start; i < tabs.size(); i++ ) {
    tabs[i] = ( ( i % 8 ) == 0 );
  }
}

DrawState::DrawState( int s_width, int s_height, int s_xpixel, int s_ypixel )
  : width( s_width ), height( s_height ), xpixel( s_xpixel ), ypixel( s_ypixel ), cursor_col( 0 ), cursor_row( 0 ),
    combining_char_col( 0 ), combining_char_row( 0 ), default_tabs( true ), tabs( s_width ),
    scrolling_region_top_row( 0 ), scrolling_region_bottom_row( height - 1 ), renditions( 0 ), hyperlink(), save(),
    next_print_will_wrap( false ), origin_mode( false ), auto_wrap_mode( true ), insert_mode( false ),
    cursor_visible( true ), reverse_video( false ), bracketed_paste( false ),
    mouse_reporting_mode( MOUSE_REPORTING_NONE ), mouse_focus_event( false ), mouse_alternate_scroll( false ),
    mouse_encoding_mode( MOUSE_ENCODING_DEFAULT ), application_mode_cursor_keys( false )
{
  reinitialize_tabs( 0 );
}

static const uint32_t KITTY_NUMBER_APP_ID_BASE = 0x80000000;

Framebuffer::Framebuffer( int s_width, int s_height )
  : rows(), icon_name(), window_title(), clipboard(), bell_count( 0 ), title_initialized( false ), kitty_images(),
    kitty_app_id_to_internal(), kitty_number_to_internal(), kitty_next_internal_id( 1 ),
    kitty_next_number_app_id( KITTY_NUMBER_APP_ID_BASE ), kitty_next_placement_uid( 1 ), kitty_admitted(),
    kitty_pending(), kitty_ids_are_internal( false ), ds( s_width, s_height )
{
  assert( s_height > 0 );
  assert( s_width > 0 );
  const size_t w = s_width;
  const color_type c = 0;
  rows = rows_type( s_height, row_pointer( std::make_shared<Row>( w, c ) ) );
}

Framebuffer::Framebuffer( const Framebuffer& other )
  : rows( other.rows ), icon_name( other.icon_name ), window_title( other.window_title ),
    clipboard( other.clipboard ), bell_count( other.bell_count ), title_initialized( other.title_initialized ),
    kitty_images( other.kitty_images ), kitty_app_id_to_internal( other.kitty_app_id_to_internal ),
    kitty_number_to_internal( other.kitty_number_to_internal ),
    kitty_next_internal_id( other.kitty_next_internal_id ),
    kitty_next_number_app_id( other.kitty_next_number_app_id ),
    kitty_next_placement_uid( other.kitty_next_placement_uid ), kitty_admitted( other.kitty_admitted ),
    kitty_pending( other.kitty_pending ), kitty_ids_are_internal( other.kitty_ids_are_internal ), ds( other.ds )
{}

Framebuffer& Framebuffer::operator=( const Framebuffer& other )
{
  if ( this != &other ) {
    rows = other.rows;
    icon_name = other.icon_name;
    window_title = other.window_title;
    clipboard = other.clipboard;
    bell_count = other.bell_count;
    title_initialized = other.title_initialized;
    kitty_images = other.kitty_images;
    kitty_app_id_to_internal = other.kitty_app_id_to_internal;
    kitty_number_to_internal = other.kitty_number_to_internal;
    kitty_next_internal_id = other.kitty_next_internal_id;
    kitty_next_number_app_id = other.kitty_next_number_app_id;
    kitty_next_placement_uid = other.kitty_next_placement_uid;
    kitty_admitted = other.kitty_admitted;
    kitty_pending = other.kitty_pending;
    kitty_ids_are_internal = other.kitty_ids_are_internal;
    ds = other.ds;
  }
  return *this;
}

void Framebuffer::scroll( int N )
{
  if ( N >= 0 ) {
    delete_line( ds.get_scrolling_region_top_row(), N );
  } else {
    insert_line( ds.get_scrolling_region_top_row(), -N );
  }
}

void DrawState::new_grapheme( void )
{
  combining_char_col = cursor_col;
  combining_char_row = cursor_row;
}

void DrawState::snap_cursor_to_border( void )
{
  if ( cursor_row < limit_top() )
    cursor_row = limit_top();
  if ( cursor_row > limit_bottom() )
    cursor_row = limit_bottom();
  if ( cursor_col < 0 )
    cursor_col = 0;
  if ( cursor_col >= width )
    cursor_col = width - 1;
}

void DrawState::move_row( int N, bool relative )
{
  if ( relative ) {
    cursor_row += N;
  } else {
    cursor_row = N + limit_top();
  }

  snap_cursor_to_border();
  new_grapheme();
  next_print_will_wrap = false;
}

void DrawState::move_col( int N, bool relative, bool implicit )
{
  if ( implicit ) {
    new_grapheme();
  }

  if ( relative ) {
    cursor_col += N;
  } else {
    cursor_col = N;
  }

  if ( implicit ) {
    next_print_will_wrap = ( cursor_col >= width );
  }

  snap_cursor_to_border();
  if ( !implicit ) {
    new_grapheme();
    next_print_will_wrap = false;
  }
}

void Framebuffer::move_rows_autoscroll( int rows )
{
  /* don't scroll if outside the scrolling region */
  if ( ( ds.get_cursor_row() < ds.get_scrolling_region_top_row() )
       || ( ds.get_cursor_row() > ds.get_scrolling_region_bottom_row() ) ) {
    ds.move_row( rows, true );
    return;
  }

  if ( ds.get_cursor_row() + rows > ds.get_scrolling_region_bottom_row() ) {
    int N = ds.get_cursor_row() + rows - ds.get_scrolling_region_bottom_row();
    scroll( N );
    ds.move_row( -N, true );
  } else if ( ds.get_cursor_row() + rows < ds.get_scrolling_region_top_row() ) {
    int N = ds.get_cursor_row() + rows - ds.get_scrolling_region_top_row();
    scroll( N );
    ds.move_row( -N, true );
  }

  ds.move_row( rows, true );
}

Cell* Framebuffer::get_combining_cell( void )
{
  if ( ( ds.get_combining_char_col() < 0 ) || ( ds.get_combining_char_row() < 0 )
       || ( ds.get_combining_char_col() >= ds.get_width() )
       || ( ds.get_combining_char_row() >= ds.get_height() ) ) {
    return NULL;
  } /* can happen if a resize came in between */

  return get_mutable_cell( ds.get_combining_char_row(), ds.get_combining_char_col() );
}

void DrawState::set_tab( void )
{
  tabs[cursor_col] = true;
}

void DrawState::clear_tab( int col )
{
  tabs[col] = false;
}

int DrawState::get_next_tab( int count ) const
{
  if ( count >= 0 ) {
    for ( int i = cursor_col + 1; i < width; i++ ) {
      if ( tabs[i] && --count == 0 ) {
        return i;
      }
    }
    return -1;
  }
  for ( int i = cursor_col - 1; i > 0; i-- ) {
    if ( tabs[i] && ++count == 0 ) {
      return i;
    }
  }
  return 0;
}

void DrawState::set_scrolling_region( int top, int bottom )
{
  if ( height < 1 ) {
    return;
  }

  scrolling_region_top_row = top;
  scrolling_region_bottom_row = bottom;

  if ( scrolling_region_top_row < 0 )
    scrolling_region_top_row = 0;
  if ( scrolling_region_bottom_row >= height )
    scrolling_region_bottom_row = height - 1;

  if ( scrolling_region_bottom_row < scrolling_region_top_row )
    scrolling_region_bottom_row = scrolling_region_top_row;
  /* real rule requires TWO-line scrolling region */

  if ( origin_mode ) {
    snap_cursor_to_border();
    new_grapheme();
  }
}

int DrawState::limit_top( void ) const
{
  return origin_mode ? scrolling_region_top_row : 0;
}

int DrawState::limit_bottom( void ) const
{
  return origin_mode ? scrolling_region_bottom_row : height - 1;
}

void Framebuffer::apply_renditions_to_cell( Cell* cell )
{
  if ( !cell ) {
    cell = get_mutable_cell();
  }
  cell->set_renditions( ds.get_renditions() );
}

void Framebuffer::apply_hyperlink_to_cell( Cell* cell )
{
  if ( !cell ) {
    cell = get_mutable_cell();
  }
  cell->set_hyperlink( ds.get_hyperlink() );
}

SavedCursor::SavedCursor()
  : cursor_col( 0 ), cursor_row( 0 ), renditions( 0 ), auto_wrap_mode( true ), origin_mode( false )
{}

void DrawState::save_cursor( void )
{
  save.cursor_col = cursor_col;
  save.cursor_row = cursor_row;
  save.renditions = renditions;
  save.auto_wrap_mode = auto_wrap_mode;
  save.origin_mode = origin_mode;
}

void DrawState::restore_cursor( void )
{
  cursor_col = save.cursor_col;
  cursor_row = save.cursor_row;
  renditions = save.renditions;
  auto_wrap_mode = save.auto_wrap_mode;
  origin_mode = save.origin_mode;

  snap_cursor_to_border(); /* we could have resized in between */
  new_grapheme();
}

void Framebuffer::insert_line( int before_row, int count )
{
  if ( ( before_row < ds.get_scrolling_region_top_row() )
       || ( before_row > ds.get_scrolling_region_bottom_row() + 1 ) ) {
    return;
  }

  int scroll = ds.get_scrolling_region_bottom_row() + 1 - before_row;
  if ( count < scroll ) {
    scroll = count;
  }

  if ( scroll == 0 ) {
    return;
  }

  // delete old rows
  rows_type::iterator start = rows.begin() + ds.get_scrolling_region_bottom_row() + 1 - scroll;
  rows.erase( start, start + scroll );
  // insert new rows
  start = rows.begin() + before_row;
  rows.insert( start, scroll, newrow() );
}

void Framebuffer::delete_line( int row, int count )
{
  if ( ( row < ds.get_scrolling_region_top_row() ) || ( row > ds.get_scrolling_region_bottom_row() ) ) {
    return;
  }

  int scroll = ds.get_scrolling_region_bottom_row() + 1 - row;
  if ( count < scroll ) {
    scroll = count;
  }

  if ( scroll == 0 ) {
    return;
  }

  // delete old rows
  rows_type::iterator start = rows.begin() + row;
  rows.erase( start, start + scroll );
  // insert a block of dummy rows
  start = rows.begin() + ds.get_scrolling_region_bottom_row() + 1 - scroll;
  rows.insert( start, scroll, newrow() );
}

Row::Row( const size_t s_width, const color_type background_color )
  : cells( s_width, Cell( background_color ) ), gen( get_gen() ), placements()
{}

uint64_t Row::get_gen() const
{
  static uint64_t gen_counter = 0;
  return gen_counter++;
}

void Row::insert_cell( int col, color_type background_color )
{
  cells.insert( cells.begin() + col, Cell( background_color ) );
  cells.pop_back();
}

void Row::delete_cell( int col, color_type background_color )
{
  cells.push_back( Cell( background_color ) );
  cells.erase( cells.begin() + col );
}

void Framebuffer::insert_cell( int row, int col )
{
  get_mutable_row( row )->insert_cell( col, ds.get_background_rendition() );
}

void Framebuffer::delete_cell( int row, int col )
{
  get_mutable_row( row )->delete_cell( col, ds.get_background_rendition() );
}

void Framebuffer::reset( void )
{
  int width = ds.get_width(), height = ds.get_height();
  ds = DrawState( width, height );
  rows = rows_type( height, newrow() ); /* fresh rows carry no placements */
  window_title.clear();
  clipboard.clear();
  kitty_images.clear();
  kitty_app_id_to_internal.clear();
  kitty_number_to_internal.clear();
  kitty_next_number_app_id = KITTY_NUMBER_APP_ID_BASE;
  kitty_admitted.clear();
  kitty_pending.clear();
  /* kitty_next_internal_id and kitty_next_placement_uid are NOT reset here:
     they must stay monotonic for the life of the session, not just this
     Framebuffer. The transport keeps older states around (sent_states,
     received_states) that can still reference images/placements by id; if
     a RIS restarted these counters at 1, a new image transmitted after RIS
     could reuse an id the client already has complete, and the client's
     kitty_apply_chunk would then treat every chunk for it as stale (or, if
     the reused id happened to still be mid-assembly, resume from the wrong
     offset). Restarting the store, its maps and the pending assembly is
     still correct -- RIS really does throw those away. */
  /* kitty_ids_are_internal is not reset either: it identifies which side of
     the wire this Framebuffer belongs to, not display state. */
  /* do not reset bell_count */
}

void Framebuffer::soft_reset( void )
{
  ds.insert_mode = false;
  ds.origin_mode = false;
  ds.cursor_visible = true; /* per xterm and gnome-terminal */
  ds.application_mode_cursor_keys = false;
  ds.set_scrolling_region( 0, ds.get_height() - 1 );
  ds.add_rendition( 0 );
  ds.set_hyperlink( Hyperlink() );
  ds.clear_saved_cursor();
}

void Framebuffer::resize( int s_width, int s_height, int s_xpixel, int s_ypixel )
{
  assert( s_width > 0 );
  assert( s_height > 0 );

  int oldheight = ds.get_height();
  int oldwidth = ds.get_width();
  ds.resize( s_width, s_height, s_xpixel, s_ypixel );

  row_pointer blankrow( newrow() );
  if ( oldheight != s_height ) {
    rows.resize( s_height, blankrow );
  }
  if ( oldwidth == s_width ) {
    return;
  }
  for ( rows_type::iterator i = rows.begin(); i != rows.end() && *i != blankrow; i++ ) {
    *i = std::make_shared<Row>( **i );
    ( *i )->set_wrap( false );
    ( *i )->cells.resize( s_width, Cell( ds.get_background_rendition() ) );
  }
}

void DrawState::resize( int s_width, int s_height, int s_xpixel, int s_ypixel )
{
  if ( ( width != s_width ) || ( height != s_height ) ) {
    /* reset entire scrolling region on any resize */
    /* xterm and rxvt-unicode do this. gnome-terminal only
       resets scrolling region if it has to become smaller in resize */
    scrolling_region_top_row = 0;
    scrolling_region_bottom_row = s_height - 1;
  }

  tabs.resize( s_width );
  if ( default_tabs ) {
    reinitialize_tabs( width );
  }

  width = s_width;
  height = s_height;
  if ( s_xpixel >= 0 ) {
    xpixel = s_xpixel;
  }
  if ( s_ypixel >= 0 ) {
    ypixel = s_ypixel;
  }

  snap_cursor_to_border();

  /* saved cursor will be snapped to border on restore */

  /* invalidate combining char cell if necessary */
  if ( ( combining_char_col >= width ) || ( combining_char_row >= height ) ) {
    combining_char_col = combining_char_row = -1;
  }
}

Renditions::Renditions( color_type s_background )
  : foreground_color( 0 ), background_color( s_background ), attributes( 0 )
{}

/* This routine cannot be used to set a color beyond the 16-color set. */
void Renditions::set_rendition( color_type num )
{
  if ( num == 0 ) {
    clear_attributes();
    foreground_color = background_color = 0;
    return;
  }

  if ( num == 39 ) {
    foreground_color = 0;
    return;
  } else if ( num == 49 ) {
    background_color = 0;
    return;
  }

  if ( ( 30 <= num ) && ( num <= 37 ) ) { /* foreground color in 8-color set */
    foreground_color = num;
    return;
  } else if ( ( 40 <= num ) && ( num <= 47 ) ) { /* background color in 8-color set */
    background_color = num;
    return;
  } else if ( ( 90 <= num ) && ( num <= 97 ) ) { /* foreground color in 16-color set */
    foreground_color = num - 90 + 38;
    return;
  } else if ( ( 100 <= num ) && ( num <= 107 ) ) { /* background color in 16-color set */
    background_color = num - 100 + 48;
    return;
  }

  bool value = num < 9;
  switch ( num ) {
    case 1:
    case 22:
      set_attribute( bold, value );
      break;
    case 3:
    case 23:
      set_attribute( italic, value );
      break;
    case 4:
    case 24:
      set_attribute( underlined, value );
      break;
    case 5:
    case 25:
      set_attribute( blink, value );
      break;
    case 7:
    case 27:
      set_attribute( inverse, value );
      break;
    case 8:
    case 28:
      set_attribute( invisible, value );
      break;
    default:
      break; /* ignore unknown rendition */
  }
}

void Renditions::set_foreground_color( int num )
{
  if ( ( 0 <= num ) && ( num <= 255 ) ) {
    foreground_color = 30 + num;
  } else if ( is_true_color( num ) ) {
    foreground_color = num;
  }
}

void Renditions::set_background_color( int num )
{
  if ( ( 0 <= num ) && ( num <= 255 ) ) {
    background_color = 40 + num;
  } else if ( is_true_color( num ) ) {
    background_color = num;
  }
}

std::string Renditions::sgr( void ) const
{
  std::string ret;
  char col[64];

  ret.append( "\033[0" );
  if ( get_attribute( bold ) )
    ret.append( ";1" );
  if ( get_attribute( italic ) )
    ret.append( ";3" );
  if ( get_attribute( underlined ) )
    ret.append( ";4" );
  if ( get_attribute( blink ) )
    ret.append( ";5" );
  if ( get_attribute( inverse ) )
    ret.append( ";7" );
  if ( get_attribute( invisible ) )
    ret.append( ";8" );

  if ( foreground_color ) {
    // Since foreground_color is a 25-bit field, it is promoted to an int when
    // manipulated. (See [conv.prom] in various C++ standards, e.g.,
    // https://timsong-cpp.github.io/cppwp/n4659/conv.prom#5.) The correct
    // printf format specifier is thus %d.
    if ( is_true_color( foreground_color ) ) {
      snprintf( col,
                sizeof( col ),
                ";38;2;%d;%d;%d",
                ( foreground_color >> 16 ) & 0xff,
                ( foreground_color >> 8 ) & 0xff,
                foreground_color & 0xff );
    } else if ( foreground_color > 37 ) { /* use 256-color set */
      snprintf( col, sizeof( col ), ";38;5;%d", foreground_color - 30 );
    } else { /* ANSI foreground color */
      // Unfortunately, some versions of GCC (notably including GCC 9.3) give
      // -Wformat warnings when relying on [conv.prom] to promote
      // foreground_color in calls to printf. Explicitly promote it to silence
      // the warning.
      int fg = foreground_color;
      snprintf( col, sizeof( col ), ";%d", fg );
    }
    ret.append( col );
  }
  if ( background_color ) {
    // See comment above about bit-field promotion; it applies here as well.
    if ( is_true_color( background_color ) ) {
      snprintf( col,
                sizeof( col ),
                ";48;2;%d;%d;%d",
                ( background_color >> 16 ) & 0xff,
                ( background_color >> 8 ) & 0xff,
                background_color & 0xff );
    } else if ( background_color > 47 ) { /* use 256-color set */
      snprintf( col, sizeof( col ), ";48;5;%d", background_color - 40 );
    } else { /* ANSI background color */
      // See comment above about explicit promotion; it applies here as well.
      int bg = background_color;
      snprintf( col, sizeof( col ), ";%d", bg );
    }
    ret.append( col );
  }
  ret.append( "m" );

  return ret;
}

bool Hyperlink::operator==( const Hyperlink& x ) const
{
  if ( rep == x.rep ) {
    return true;
  }
  if ( rep == nullptr || x.rep == nullptr ) {
    return false;
  }

  return rep->url == x.rep->url && rep->params == x.rep->params;
}

std::string Hyperlink::osc8() const
{
  std::string ret;

  ret.append( "\033]8;" );

  if ( *this )
    ret.append( rep->params );
  ret.append( ";" );
  if ( *this )
    ret.append( rep->url );

  ret.append( "\033\\" );
  return ret;
}

void Row::reset( color_type background_color )
{
  gen = get_gen();
  for ( cells_type::iterator i = cells.begin(); i != cells.end(); i++ ) {
    i->reset( background_color );
  }
  placements.clear();
}

static bool placements_equal( const std::vector<std::shared_ptr<const ImagePlacement>>& a,
                              const std::vector<std::shared_ptr<const ImagePlacement>>& b )
{
  if ( a.size() != b.size() ) {
    return false;
  }
  for ( size_t i = 0; i < a.size(); i++ ) {
    if ( !( *a[i] == *b[i] ) ) {
      return false;
    }
  }
  return true;
}

bool Row::operator==( const Row& x ) const
{
  return ( gen == x.gen ) && ( cells == x.cells ) && placements_equal( placements, x.placements );
}

void Framebuffer::prefix_window_title( const title_type& s )
{
  if ( icon_name == window_title ) {
    /* preserve equivalence */
    icon_name.insert( icon_name.begin(), s.begin(), s.end() );
  }
  window_title.insert( window_title.begin(), s.begin(), s.end() );
}

static const size_t KITTY_DEFAULT_STORE_CAP = 64 * 1024 * 1024;
static size_t kitty_store_cap = KITTY_DEFAULT_STORE_CAP;

void Framebuffer::set_kitty_store_cap_for_tests( size_t bytes )
{
  kitty_store_cap = bytes;
}

uint32_t Framebuffer::image_store_put( uint32_t app_id,
                                       int format,
                                       int width,
                                       int height,
                                       bool compressed,
                                       std::shared_ptr<const std::string> blob )
{
  if ( app_id != 0 ) {
    std::map<uint32_t, uint32_t>::iterator existing = kitty_app_id_to_internal.find( app_id );
    if ( existing != kitty_app_id_to_internal.end() ) {
      /* re-transmit: the old internal id's placements, data, app-id mapping
         and any number mapping pointing at it are all gone. */
      uint32_t old_internal_id = existing->second;
      delete_placements_of_image( old_internal_id );
      image_store_forget( old_internal_id );
    }
  }

  uint32_t internal_id = kitty_next_internal_id++;
  auto image = std::make_shared<Image>( internal_id, app_id, format, width, height, compressed, std::move( blob ) );

  kitty_images[internal_id] = image;
  kitty_admitted[internal_id] = 0;
  if ( app_id != 0 ) {
    kitty_app_id_to_internal[app_id] = internal_id;
  }
  return internal_id;
}

void Framebuffer::image_store_put_with_id( uint32_t internal_id,
                                           int format,
                                           int width,
                                           int height,
                                           bool compressed )
{
  auto image = std::make_shared<Image>(
    internal_id, /* app_id = */ 0, format, width, height, compressed, /* blob = */ nullptr );
  kitty_images[internal_id] = image;
  kitty_admitted[internal_id] = 0;
  if ( internal_id >= kitty_next_internal_id ) {
    kitty_next_internal_id = internal_id + 1;
  }
}

std::shared_ptr<const Image> Framebuffer::image_store_get( uint32_t internal_id ) const
{
  std::map<uint32_t, std::shared_ptr<const Image>>::const_iterator it = kitty_images.find( internal_id );
  return it == kitty_images.end() ? nullptr : it->second;
}

uint32_t Framebuffer::image_store_resolve( uint32_t app_id ) const
{
  std::map<uint32_t, uint32_t>::const_iterator it = kitty_app_id_to_internal.find( app_id );
  return it == kitty_app_id_to_internal.end() ? 0 : it->second;
}

size_t Framebuffer::image_store_bytes( void ) const
{
  size_t total = 0;
  for ( std::map<uint32_t, std::shared_ptr<const Image>>::const_iterator it = kitty_images.begin();
        it != kitty_images.end();
        ++it ) {
    if ( it->second->blob ) {
      total += it->second->blob->size();
    }
  }
  return total;
}

void Framebuffer::image_store_forget( uint32_t internal_id )
{
  kitty_images.erase( internal_id );
  kitty_admitted.erase( internal_id );
  kitty_pending.erase( internal_id );
  for ( std::map<uint32_t, uint32_t>::iterator it = kitty_app_id_to_internal.begin();
        it != kitty_app_id_to_internal.end(); ) {
    if ( it->second == internal_id ) {
      it = kitty_app_id_to_internal.erase( it );
    } else {
      ++it;
    }
  }
  for ( std::map<uint32_t, uint32_t>::iterator it = kitty_number_to_internal.begin();
        it != kitty_number_to_internal.end(); ) {
    if ( it->second == internal_id ) {
      it = kitty_number_to_internal.erase( it );
    } else {
      ++it;
    }
  }
}

uint32_t Framebuffer::image_store_resolve_number( uint32_t number ) const
{
  std::map<uint32_t, uint32_t>::const_iterator it = kitty_number_to_internal.find( number );
  return it == kitty_number_to_internal.end() ? 0 : it->second;
}

void Framebuffer::image_store_set_number( uint32_t number, uint32_t internal_id )
{
  kitty_number_to_internal[number] = internal_id; /* latest wins */
}

uint32_t Framebuffer::kitty_allocate_app_id_for_number( void )
{
  /* An app is free to choose any i= up to 4294967295, including ids at or
     above KITTY_NUMBER_APP_ID_BASE, so blindly handing out the next counter
     value could silently steal and destroy an app-chosen image with the
     same id. Search for the next id that both is nonzero and isn't already
     claimed, wrapping around 0xFFFFFFFF back to the base of the range. */
  uint32_t start = kitty_next_number_app_id;
  uint32_t candidate = start;
  do {
    if ( ( candidate != 0 ) && ( kitty_app_id_to_internal.find( candidate ) == kitty_app_id_to_internal.end() ) ) {
      kitty_next_number_app_id = ( candidate == 0xFFFFFFFFu ) ? KITTY_NUMBER_APP_ID_BASE : candidate + 1;
      return candidate;
    }
    candidate = ( candidate == 0xFFFFFFFFu ) ? KITTY_NUMBER_APP_ID_BASE : candidate + 1;
  } while ( candidate != start );
  return 0; /* every id in the range is taken; caller replies ENOSPC */
}

bool Framebuffer::image_store_make_room( uint32_t replacing_app_id, size_t incoming_bytes )
{
  /* An image already mapped to replacing_app_id is about to be replaced by
     image_store_put: credit its bytes and its slot back so a same-size (or
     smaller) re-transmit of the same id never has to evict anything, and is
     never blocked by the count cap either. Never evict it out from under
     ourselves while computing that credit. */
  uint32_t protected_id = 0;
  if ( replacing_app_id != 0 ) {
    std::map<uint32_t, uint32_t>::const_iterator it = kitty_app_id_to_internal.find( replacing_app_id );
    if ( it != kitty_app_id_to_internal.end() ) {
      protected_id = it->second;
    }
  }

  auto bytes_excluding_protected = [&]() -> size_t {
    size_t total = 0;
    for ( std::map<uint32_t, std::shared_ptr<const Image>>::const_iterator it = kitty_images.begin();
          it != kitty_images.end();
          ++it ) {
      if ( it->first == protected_id ) {
        continue;
      }
      if ( it->second->blob ) {
        total += it->second->blob->size();
      }
    }
    return total;
  };
  auto count_excluding_protected = [&]() -> size_t { return kitty_images.size() - ( protected_id != 0 ? 1 : 0 ); };

  while ( ( bytes_excluding_protected() + incoming_bytes > kitty_store_cap )
          || ( count_excluding_protected() + 1 > Kitty::MAX_IMAGES ) ) {
    uint32_t victim = 0;
    for ( std::map<uint32_t, std::shared_ptr<const Image>>::const_iterator it = kitty_images.begin();
          it != kitty_images.end();
          ++it ) {
      if ( it->first == protected_id ) {
        continue;
      }
      if ( !image_has_placement( it->first ) ) {
        victim = it->first;
        break;
      }
    }
    if ( victim == 0 ) {
      return false; /* nothing left that can be evicted; still would not fit */
    }
    image_store_forget( victim );
  }
  return true;
}

size_t Framebuffer::kitty_admitted_bytes( uint32_t internal_id ) const
{
  std::map<uint32_t, size_t>::const_iterator it = kitty_admitted.find( internal_id );
  return it == kitty_admitted.end() ? 0 : it->second;
}

size_t Framebuffer::kitty_admit_bytes( size_t budget )
{
  size_t admitted_total = 0;
  /* kitty_images is a std::map, so this walks internal ids in order. */
  for ( std::map<uint32_t, std::shared_ptr<const Image>>::const_iterator it = kitty_images.begin();
        it != kitty_images.end() && budget > 0;
        ++it ) {
    const uint32_t internal_id = it->first;
    const size_t blob_size = it->second->blob ? it->second->blob->size() : 0;
    size_t& admitted = kitty_admitted[internal_id]; /* image_store_put always seeded this at 0 */
    if ( admitted >= blob_size ) {
      continue;
    }
    const size_t room = blob_size - admitted;
    const size_t take = ( room < budget ) ? room : budget;
    admitted += take;
    budget -= take;
    admitted_total += take;
  }
  return admitted_total;
}

bool Framebuffer::kitty_has_unadmitted_bytes( void ) const
{
  for ( std::map<uint32_t, std::shared_ptr<const Image>>::const_iterator it = kitty_images.begin();
        it != kitty_images.end();
        ++it ) {
    const size_t blob_size = it->second->blob ? it->second->blob->size() : 0;
    if ( kitty_admitted_bytes( it->first ) < blob_size ) {
      return true;
    }
  }
  return false;
}

void Framebuffer::kitty_apply_chunk( uint32_t internal_id,
                                     uint64_t offset,
                                     std::shared_ptr<const std::string> data,
                                     uint64_t total,
                                     int format,
                                     int width,
                                     int height,
                                     bool compressed )
{
  std::map<uint32_t, std::shared_ptr<const Image>>::const_iterator it = kitty_images.find( internal_id );
  if ( it != kitty_images.end() && it->second->blob ) {
    return; /* already complete: a full repaint re-sends everything, so this chunk is stale */
  }

  std::map<uint32_t, KittyPendingImage>::iterator pending_it = kitty_pending.find( internal_id );

  if ( pending_it == kitty_pending.end() ) {
    /* First chunk for this id: its metadata locks in the image's identity
       for every later chunk of this id (checked in the else branch below).
       The client never evicts (unlike the server, it has nowhere else to
       get the bytes back from), so a chunk that would blow a cap, or
       already overruns its own declared total, is refused outright rather
       than accepted on faith from the network. */
    if ( offset != 0 ) {
      return; /* the assembly must start at 0 */
    }
    if ( total > Kitty::IMAGE_MAX_BYTES ) {
      return;
    }
    if ( data->size() > total ) {
      return; /* the first piece alone already overruns the declared total */
    }
    if ( ( kitty_images.size() + 1 > Kitty::MAX_IMAGES )
         || ( image_store_bytes() + kitty_pending_reserved_bytes() + total > kitty_store_cap ) ) {
      return;
    }
    image_store_put_with_id( internal_id, format, width, height, compressed );
    pending_it
      = kitty_pending.emplace( internal_id, KittyPendingImage( total, format, width, height, compressed ) ).first;
  } else {
    /* Continuation: must agree with the identity the first chunk locked
       in, and be exactly the next contiguous, in-budget piece. Anything
       else is untrusted network input: drop it, no state change. */
    const KittyPendingImage& pending = pending_it->second;
    if ( ( total != pending.total ) || ( format != pending.format ) || ( width != pending.width )
         || ( height != pending.height ) || ( compressed != pending.compressed ) ) {
      return;
    }
    if ( offset != pending.received ) {
      return;
    }
    if ( data->size() > pending.total - pending.received ) {
      return;
    }
  }

  KittyPendingImage& pending = pending_it->second;
  if ( !data->empty() ) {
    pending.received += data->size();
    pending.pieces.push_back( std::move( data ) );
  }

  if ( pending.received < pending.total ) {
    return; /* still waiting for more pieces */
  }

  /* pending.received == pending.total exactly: every chunk above that
     would have made it overshoot was already rejected. */
  std::string full;
  full.reserve( pending.total );
  for ( const auto& piece : pending.pieces ) {
    full += *piece;
  }

  std::shared_ptr<const Image> stub = kitty_images.at( internal_id );
  kitty_images[internal_id] = std::make_shared<Image>( internal_id,
                                                       stub->app_id,
                                                       stub->format,
                                                       stub->width,
                                                       stub->height,
                                                       stub->compressed,
                                                       std::make_shared<const std::string>( std::move( full ) ) );
  kitty_pending.erase( internal_id );
}

size_t Framebuffer::kitty_pending_reserved_bytes( void ) const
{
  size_t total_reserved = 0;
  for ( const auto& kv : kitty_pending ) {
    total_reserved += kv.second.total;
  }
  return total_reserved;
}

size_t Framebuffer::kitty_pending_piece_count( uint32_t internal_id ) const
{
  std::map<uint32_t, KittyPendingImage>::const_iterator it = kitty_pending.find( internal_id );
  return it == kitty_pending.end() ? 0 : it->second.pieces.size();
}

void Framebuffer::add_placement( int row, std::shared_ptr<ImagePlacement> placement, uint32_t forced_uid )
{
  placement->uid = forced_uid ? forced_uid : kitty_next_placement_uid++;
  Row* mutable_row = get_mutable_row( row );
  mutable_row->placements.push_back( std::move( placement ) );
  /* A placements-only change doesn't touch cells, so it must bump gen itself
     (like Row::reset() does for ED/EL) -- otherwise this row can coincide,
     cell-for-cell and gen-for-gen, with an unrelated row that has always
     been blank (both trace back to the same shared blank-row prototype),
     and Display::new_frame's scroll shortcut will treat the two as the same
     row and skip diffing this one's placements entirely. */
  mutable_row->gen = mutable_row->get_gen();
}

bool Framebuffer::image_has_placement( uint32_t internal_id ) const
{
  for ( rows_type::const_iterator r = rows.begin(); r != rows.end(); ++r ) {
    for ( const auto& p : ( *r )->placements ) {
      if ( p->internal_image_id == internal_id ) {
        return true;
      }
    }
  }
  return false;
}

bool Framebuffer::placement_exists( uint32_t internal_id, uint32_t placement_id ) const
{
  for ( rows_type::const_iterator r = rows.begin(); r != rows.end(); ++r ) {
    for ( const auto& p : ( *r )->placements ) {
      if ( ( p->internal_image_id == internal_id ) && ( p->placement_id == placement_id ) ) {
        return true;
      }
    }
  }
  return false;
}

size_t Framebuffer::placement_count( void ) const
{
  size_t total = 0;
  for ( rows_type::const_iterator r = rows.begin(); r != rows.end(); ++r ) {
    total += ( *r )->placements.size();
  }
  return total;
}

void Framebuffer::delete_placements_of_image( uint32_t internal_id )
{
  for ( size_t r = 0; r < rows.size(); r++ ) {
    bool has = false;
    for ( const auto& p : rows[r]->placements ) {
      if ( p->internal_image_id == internal_id ) {
        has = true;
        break;
      }
    }
    if ( !has ) {
      continue;
    }
    Row* mutable_row = get_mutable_row( static_cast<int>( r ) );
    auto& placements = mutable_row->placements;
    placements.erase( std::remove_if( placements.begin(),
                                      placements.end(),
                                      [internal_id]( const std::shared_ptr<const ImagePlacement>& p ) {
                                        return p->internal_image_id == internal_id;
                                      } ),
                      placements.end() );
    mutable_row->gen = mutable_row->get_gen(); /* see add_placement */
  }
}

void Framebuffer::delete_placement_by_id( uint32_t internal_id, uint32_t placement_id )
{
  for ( size_t r = 0; r < rows.size(); r++ ) {
    bool has = false;
    for ( const auto& p : rows[r]->placements ) {
      if ( ( p->internal_image_id == internal_id ) && ( p->placement_id == placement_id ) ) {
        has = true;
        break;
      }
    }
    if ( !has ) {
      continue;
    }
    Row* mutable_row = get_mutable_row( static_cast<int>( r ) );
    auto& placements = mutable_row->placements;
    placements.erase(
      std::remove_if( placements.begin(),
                      placements.end(),
                      [internal_id, placement_id]( const std::shared_ptr<const ImagePlacement>& p ) {
                        return ( p->internal_image_id == internal_id ) && ( p->placement_id == placement_id );
                      } ),
      placements.end() );
    mutable_row->gen = mutable_row->get_gen(); /* see add_placement */
  }
}

void Framebuffer::clear_all_placements( bool free_data )
{
  for ( size_t r = 0; r < rows.size(); r++ ) {
    if ( rows[r]->placements.empty() ) {
      continue;
    }
    Row* mutable_row = get_mutable_row( static_cast<int>( r ) );
    mutable_row->placements.clear();
    mutable_row->gen = mutable_row->get_gen(); /* see add_placement */
  }
  if ( free_data ) {
    kitty_images.clear();
    kitty_app_id_to_internal.clear();
    kitty_number_to_internal.clear();
    /* Every historical id's admitted-bytes and pending-assembly entries
       must go too: this clears the whole store directly rather than
       through image_store_forget (which drops both per id), and those
       maps would otherwise keep growing, entry by entry, across however
       many images this Framebuffer ever held -- and get copied into every
       transport snapshot along with it. */
    kitty_admitted.clear();
    kitty_pending.clear();
  }
}

std::string Cell::debug_contents( void ) const
{
  if ( contents.empty() ) {
    return "'_' ()";
  }
  std::string chars( 1, '\'' );
  print_grapheme( chars );
  chars.append( "' [" );
  const char* lazycomma = "";
  char buf[64];
  for ( content_type::const_iterator i = contents.begin(); i < contents.end(); i++ ) {

    snprintf( buf, sizeof buf, "%s0x%02x", lazycomma, static_cast<uint8_t>( *i ) );
    chars.append( buf );
    lazycomma = ", ";
  }
  chars.append( "]" );
  return chars;
}

bool Cell::compare( const Cell& other ) const
{
  bool ret = false;

  std::string grapheme, other_grapheme;

  print_grapheme( grapheme );
  other.print_grapheme( other_grapheme );

  if ( grapheme != other_grapheme ) {
    ret = true;
    fprintf( stderr, "Graphemes: '%s' vs. '%s'\n", grapheme.c_str(), other_grapheme.c_str() );
  }

  if ( !contents_match( other ) ) {
    // ret = true;
    fprintf( stderr,
             "Contents: %s (%ld) vs. %s (%ld)\n",
             debug_contents().c_str(),
             static_cast<long int>( contents.size() ),
             other.debug_contents().c_str(),
             static_cast<long int>( other.contents.size() ) );
  }

  if ( fallback != other.fallback ) {
    // ret = true;
    // Since fallback is a 1-bit field, it is promoted to an int when
    // manipulated. (See [conv.prom] in various C++ standards, e.g.,
    // https://timsong-cpp.github.io/cppwp/n4659/conv.prom#5.) The correct
    // printf format specifier is thus %d.
    fprintf( stderr, "fallback: %d vs. %d\n", fallback, other.fallback );
  }

  if ( wide != other.wide ) {
    ret = true;
    // See comment above about bit-field promotion; it applies here as well.
    fprintf( stderr, "width: %d vs. %d\n", wide, other.wide );
  }

  if ( !( renditions == other.renditions ) ) {
    ret = true;
    fprintf( stderr, "renditions differ\n" );
  }

  if ( wrap != other.wrap ) {
    ret = true;
    // See comment above about bit-field promotion; it applies here as well.
    fprintf( stderr, "wrap: %d vs. %d\n", wrap, other.wrap );
  }

  return ret;
}

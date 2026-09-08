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

#include "src/terminal/terminalreply.h"

using namespace Terminal;

namespace {
const char* const CSI_997_LITERAL = "997";
const char* const RGB_LITERAL = "rgb:";
}

bool TerminalReplyFilter::is_hex_digit( unsigned char c )
{
  return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
}

void TerminalReplyFilter::reset_accumulators( void )
{
  csi_lit_idx_ = 0;
  n_digits_.clear();
  osc_lit_idx_ = 0;
  rgb_r_.clear();
  rgb_g_.clear();
  rgb_b_.clear();
}

void TerminalReplyFilter::flush( std::string& passthrough )
{
  passthrough += held_;
  held_.clear();
  reset_accumulators();
  state_ = GROUND;
}

void TerminalReplyFilter::mismatch( unsigned char c, std::string& passthrough )
{
  /* Release everything held so far (it didn't turn out to be a recognised
     reply), then re-process c as though it were freshly read at GROUND --
     it may itself begin a new escape sequence, or just be an ordinary byte. */
  passthrough += held_;
  held_.clear();
  reset_accumulators();
  state_ = GROUND;
  process_byte( c, passthrough );
}

void TerminalReplyFilter::process_byte( unsigned char c, std::string& passthrough )
{
  switch ( state_ ) {
    case GROUND:
      if ( c == 0x1b ) {
        held_.push_back( char( c ) );
        state_ = ESC1;
      } else {
        passthrough.push_back( char( c ) );
      }
      break;

    case ESC1:
      if ( c == '[' ) {
        held_.push_back( char( c ) );
        state_ = CSI_Q;
      } else if ( c == ']' ) {
        held_.push_back( char( c ) );
        state_ = OSC_NUM1;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case CSI_Q:
      if ( c == '?' ) {
        held_.push_back( char( c ) );
        csi_lit_idx_ = 0;
        state_ = CSI_997;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case CSI_997:
      if ( c == static_cast<unsigned char>( CSI_997_LITERAL[csi_lit_idx_] ) ) {
        held_.push_back( char( c ) );
        csi_lit_idx_++;
        if ( CSI_997_LITERAL[csi_lit_idx_] == '\0' ) {
          state_ = CSI_SEMI;
        }
      } else {
        mismatch( c, passthrough );
      }
      break;

    case CSI_SEMI:
      if ( c == ';' ) {
        held_.push_back( char( c ) );
        n_digits_.clear();
        state_ = CSI_NDIGIT;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case CSI_NDIGIT:
      if ( c >= '0' && c <= '9' ) {
        held_.push_back( char( c ) );
        n_digits_.push_back( char( c ) );
      } else if ( c == 'n' ) {
        held_.push_back( char( c ) );
        if ( n_digits_ == "1" || n_digits_ == "2" ) {
          Reply r;
          r.kind = Reply::COLOR_SCHEME;
          r.scheme = ( n_digits_ == "1" ) ? 1 : 2;
          replies_.push_back( r );
          held_.clear();
          reset_accumulators();
        } else {
          /* Recognised shape, but not a scheme value we understand:
             pass the whole thing through unchanged. */
          passthrough += held_;
          held_.clear();
          reset_accumulators();
        }
        state_ = GROUND;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_NUM1:
      if ( c == '1' ) {
        held_.push_back( char( c ) );
        state_ = OSC_NUM2;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_NUM2:
      if ( c == '0' || c == '1' ) {
        held_.push_back( char( c ) );
        osc_kind_ = ( c == '0' ) ? Reply::FOREGROUND : Reply::BACKGROUND;
        state_ = OSC_SEMI;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_SEMI:
      if ( c == ';' ) {
        held_.push_back( char( c ) );
        osc_lit_idx_ = 0;
        state_ = OSC_RGB_LIT;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_RGB_LIT:
      if ( c == static_cast<unsigned char>( RGB_LITERAL[osc_lit_idx_] ) ) {
        held_.push_back( char( c ) );
        osc_lit_idx_++;
        if ( RGB_LITERAL[osc_lit_idx_] == '\0' ) {
          rgb_r_.clear();
          state_ = OSC_R;
        }
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_R:
      if ( is_hex_digit( c ) && rgb_r_.size() < 4 ) {
        held_.push_back( char( c ) );
        rgb_r_.push_back( char( c ) );
      } else if ( c == '/' && !rgb_r_.empty() ) {
        held_.push_back( char( c ) );
        rgb_g_.clear();
        state_ = OSC_G;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_G:
      if ( is_hex_digit( c ) && rgb_g_.size() < 4 ) {
        held_.push_back( char( c ) );
        rgb_g_.push_back( char( c ) );
      } else if ( c == '/' && !rgb_g_.empty() ) {
        held_.push_back( char( c ) );
        rgb_b_.clear();
        state_ = OSC_B;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_B:
      if ( is_hex_digit( c ) && rgb_b_.size() < 4 ) {
        held_.push_back( char( c ) );
        rgb_b_.push_back( char( c ) );
      } else if ( c == 0x07 && !rgb_b_.empty() ) {
        held_.push_back( char( c ) );
        Reply r;
        r.kind = osc_kind_;
        r.color = rgb_r_ + "/" + rgb_g_ + "/" + rgb_b_;
        r.scheme = 0;
        replies_.push_back( r );
        held_.clear();
        reset_accumulators();
        state_ = GROUND;
      } else if ( c == 0x1b && !rgb_b_.empty() ) {
        held_.push_back( char( c ) );
        state_ = OSC_TERM_ESC;
      } else {
        mismatch( c, passthrough );
      }
      break;

    case OSC_TERM_ESC:
      if ( c == '\\' ) {
        held_.push_back( char( c ) );
        Reply r;
        r.kind = osc_kind_;
        r.color = rgb_r_ + "/" + rgb_g_ + "/" + rgb_b_;
        r.scheme = 0;
        replies_.push_back( r );
        held_.clear();
        reset_accumulators();
        state_ = GROUND;
      } else {
        mismatch( c, passthrough );
      }
      break;
  }

  if ( held_.size() > MAX_HELD ) {
    flush( passthrough );
  }
}

void TerminalReplyFilter::feed( const std::string& bytes, std::string& passthrough )
{
  for ( size_t i = 0; i < bytes.size(); i++ ) {
    process_byte( static_cast<unsigned char>( bytes[i] ), passthrough );
  }
}

std::vector<TerminalReplyFilter::Reply> TerminalReplyFilter::take_replies( void )
{
  std::vector<Reply> out;
  out.swap( replies_ );
  return out;
}

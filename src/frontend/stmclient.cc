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

#include "src/include/config.h"

#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <err.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#if HAVE_PTY_H
#include <pty.h>
#elif HAVE_UTIL_H
#include <util.h>
#endif

#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/util/fatal_assert.h"
#include "src/util/locale_utils.h"
#include "src/util/pty_compat.h"
#include "src/util/select.h"
#include "src/util/swrite.h"
#include "src/util/timestamp.h"
#include "stmclient.h"

#include "src/network/networktransport-impl.h"

/* Ask the local terminal to notify us of light/dark scheme changes (mode 2031),
   and ask once for the current scheme and default foreground/background colours.
   The terminal replies are filtered out of the keystroke stream by reply_filter. */
static const char* const THEME_PROBE = "\033[?2031h\033[?996n\033]10;?\033\\\033]11;?\033\\";

/* Just the colour half of the probe above -- resent whenever the scheme changes,
   so the default colours we report follow suit. */
static const char* const THEME_COLOR_QUERY = "\033]10;?\033\\\033]11;?\033\\";

void STMClient::resume( void )
{
  /* Restore termios state */
  if ( tcsetattr( STDIN_FILENO, TCSANOW, &raw_termios ) < 0 ) {
    perror( "tcsetattr" );
    exit( 1 );
  }

  /* Put terminal in application-cursor-key mode */
  swrite( STDOUT_FILENO, display.open().c_str() );

  /* Re-arm theme notifications and re-query the current theme */
  swrite( STDOUT_FILENO, THEME_PROBE );

  /* Flag that outer terminal state is unknown */
  repaint_requested = true;
}

void STMClient::init( void )
{
  if ( !is_utf8_locale() ) {
    LocaleVar native_ctype = get_ctype();
    std::string native_charset( locale_charset() );

    fprintf( stderr,
             "mosh-client needs a UTF-8 native locale to run.\n\n"
             "Unfortunately, the client's environment (%s) specifies\n"
             "the character set \"%s\".\n\n",
             native_ctype.str().c_str(),
             native_charset.c_str() );
    int unused __attribute( ( unused ) ) = system( "locale" );
    exit( 1 );
  }

  /* Verify terminal configuration */
  if ( tcgetattr( STDIN_FILENO, &saved_termios ) < 0 ) {
    perror( "tcgetattr" );
    exit( 1 );
  }

  /* Put terminal driver in raw mode */
  raw_termios = saved_termios;

#ifdef HAVE_IUTF8
  if ( !( raw_termios.c_iflag & IUTF8 ) ) {
    //    fprintf( stderr, "Warning: Locale is UTF-8 but termios IUTF8 flag not set. Setting IUTF8 flag.\n" );
    /* Probably not really necessary since we are putting terminal driver into raw mode anyway. */
    raw_termios.c_iflag |= IUTF8;
  }
#endif /* HAVE_IUTF8 */

  cfmakeraw( &raw_termios );

  if ( tcsetattr( STDIN_FILENO, TCSANOW, &raw_termios ) < 0 ) {
    perror( "tcsetattr" );
    exit( 1 );
  }

  /* Put terminal in application-cursor-key mode */
  swrite( STDOUT_FILENO, display.open().c_str() );

  /* Ask the local terminal for its light/dark scheme and default colours,
     and ask it to keep us posted on scheme changes. */
  swrite( STDOUT_FILENO, THEME_PROBE );

  /* Add our name to window title */
  if ( !getenv( "MOSH_TITLE_NOPREFIX" ) ) {
    overlays.set_title_prefix( std::wstring( L"[mosh] " ) );
  }

  /* Set terminal escape key. */
  const char* escape_key_env;
  if ( ( escape_key_env = getenv( "MOSH_ESCAPE_KEY" ) ) != NULL ) {
    if ( strlen( escape_key_env ) == 1 ) {
      escape_key = (int)escape_key_env[0];
      if ( escape_key > 0 && escape_key < 128 ) {
        if ( escape_key < 32 ) {
          /* If escape is ctrl-something, pass it with repeating the key without ctrl. */
          escape_pass_key = escape_key + (int)'@';
        } else {
          /* If escape is something else, pass it with repeating the key itself. */
          escape_pass_key = escape_key;
        }
        if ( escape_pass_key >= 'A' && escape_pass_key <= 'Z' ) {
          /* If escape pass is an upper case character, define optional version
             as lower case of the same. */
          escape_pass_key2 = escape_pass_key + (int)'a' - (int)'A';
        } else {
          escape_pass_key2 = escape_pass_key;
        }
      } else {
        escape_key = 0x1E;
        escape_pass_key = '^';
        escape_pass_key2 = '^';
      }
    } else if ( strlen( escape_key_env ) == 0 ) {
      escape_key = -1;
    } else {
      escape_key = 0x1E;
      escape_pass_key = '^';
      escape_pass_key2 = '^';
    }
  } else {
    escape_key = 0x1E;
    escape_pass_key = '^';
    escape_pass_key2 = '^';
  }

  /* There are so many better ways to shoot oneself into leg than
     setting escape key to Ctrl-C, Ctrl-D, NewLine, Ctrl-L or CarriageReturn
     that we just won't allow that. */
  if ( escape_key == 0x03 || escape_key == 0x04 || escape_key == 0x0A || escape_key == 0x0C
       || escape_key == 0x0D ) {
    escape_key = 0x1E;
    escape_pass_key = '^';
    escape_pass_key2 = '^';
  }

  /* Adjust escape help differently if escape is a control character. */
  if ( escape_key > 0 ) {
    char escape_pass_name_buf[16];
    char escape_key_name_buf[16];
    snprintf( escape_pass_name_buf, sizeof escape_pass_name_buf, "\"%c\"", escape_pass_key );
    if ( escape_key < 32 ) {
      snprintf( escape_key_name_buf, sizeof escape_key_name_buf, "Ctrl-%c", escape_pass_key );
      escape_requires_lf = false;
    } else {
      snprintf( escape_key_name_buf, sizeof escape_key_name_buf, "\"%c\"", escape_key );
      escape_requires_lf = true;
    }
    std::string tmp;
    tmp = std::string( escape_pass_name_buf );
    std::wstring escape_pass_name = std::wstring( tmp.begin(), tmp.end() );
    tmp = std::string( escape_key_name_buf );
    std::wstring escape_key_name = std::wstring( tmp.begin(), tmp.end() );
    escape_key_help
      = L"Commands: Ctrl-Z suspends, \".\" quits, " + escape_pass_name + L" gives literal " + escape_key_name;
    overlays.get_notification_engine().set_escape_key_string( tmp );
  }
  wchar_t tmp[128];
  swprintf( tmp, 128, L"Nothing received from server on UDP port %s.", port.c_str() );
  connecting_notification = std::wstring( tmp );
}

void STMClient::shutdown( void )
{
  /* Restore screen state */
  overlays.get_notification_engine().set_notification_string( std::wstring( L"" ) );
  overlays.get_notification_engine().server_heard( timestamp() );
  overlays.set_title_prefix( std::wstring( L"" ) );
  output_new_frame();

  /* Restore terminal and terminal-driver state */
  swrite( STDOUT_FILENO, display.close().c_str() );

  if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
    perror( "tcsetattr" );
    exit( 1 );
  }

  if ( still_connecting() ) {
    fprintf( stderr,
             "\nmosh did not make a successful connection to %s:%s.\n"
             "Please verify that UDP port %s is not firewalled and can reach the server.\n\n"
             "(By default, mosh uses a UDP port between 60000 and 61000. The -p option\n"
             "selects a specific UDP port number.)\n",
             ip.c_str(),
             port.c_str(),
             port.c_str() );
  } else if ( network && !clean_shutdown ) {
    fputs( "\n\nmosh did not shut down cleanly. Please note that the\n"
           "mosh-server process may still be running on the server.\n",
           stderr );
  }
}

void STMClient::main_init( void )
{
  Select& sel = Select::get_instance();
  sel.add_signal( SIGWINCH );
  sel.add_signal( SIGTERM );
  sel.add_signal( SIGINT );
  sel.add_signal( SIGHUP );
  sel.add_signal( SIGPIPE );
  sel.add_signal( SIGCONT );

  /* get initial window size */
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    return;
  }

  /* local state */
  local_framebuffer = Terminal::Framebuffer( window_size.ws_col, window_size.ws_row );
  new_state = Terminal::Framebuffer( 1, 1 );

  /* initialize screen */
  std::string init = display.new_frame( false, local_framebuffer, local_framebuffer );
  swrite( STDOUT_FILENO, init.data(), init.size() );

  /* open network */
  Network::UserStream blank;
  Terminal::Complete local_terminal( window_size.ws_col, window_size.ws_row );
  network = NetworkPointer( new NetworkType( blank, local_terminal, key.c_str(), ip.c_str(), port.c_str() ) );

  network->set_send_delay( 1 ); /* minimal delay on outgoing keystrokes */

  /* tell server the size of the terminal */
  network->get_current_state().push_back(
    Parser::Resize( window_size.ws_col, window_size.ws_row, window_size.ws_xpixel, window_size.ws_ypixel ) );

  /* be noisy as necessary */
  network->set_verbose( verbose );
  Select::set_verbose( verbose );
}

void STMClient::output_new_frame( void )
{
  if ( !network ) { /* clean shutdown even when not initialized */
    return;
  }

  /* fetch target state */
  new_state = network->get_latest_remote_state().state.get_fb();

  /* apply local overlays */
  overlays.apply( new_state );

  /* calculate minimal difference from where we are */
  const std::string diff( display.new_frame( !repaint_requested, local_framebuffer, new_state ) );
  swrite( STDOUT_FILENO, diff.data(), diff.size() );

  repaint_requested = false;

  local_framebuffer = new_state;
}

void STMClient::process_network_input( void )
{
  network->recv();

  /* Now give hints to the overlays */
  overlays.get_notification_engine().server_heard( network->get_latest_remote_state().timestamp );
  overlays.get_notification_engine().server_acked( network->get_sent_state_acked_timestamp() );

  overlays.get_prediction_engine().set_local_frame_acked( network->get_sent_state_acked() );
  overlays.get_prediction_engine().set_send_interval( network->send_interval() );
  overlays.get_prediction_engine().set_local_frame_late_acked(
    network->get_latest_remote_state().state.get_echo_ack() );
}

STMClient::InputAction STMClient::process_input_byte( char the_byte )
{
  NetworkType& net = *network;

  if ( quit_sequence_started ) {
    if ( the_byte == '.' ) { /* Quit sequence is Ctrl-^ . */
      if ( net.has_remote_addr() && ( !net.shutdown_in_progress() ) ) {
        overlays.get_notification_engine().set_notification_string( std::wstring( L"Exiting on user request..." ),
                                                                    true );
        net.start_shutdown();
        return InputAction::STOP_OK;
      }
      return InputAction::STOP_EOF;
    } else if ( the_byte == 0x1a ) { /* Suspend sequence is escape_key Ctrl-Z */
      /* Restore terminal and terminal-driver state */
      swrite( STDOUT_FILENO, display.close().c_str() );

      if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
        perror( "tcsetattr" );
        exit( 1 );
      }

      fputs( "\n\033[37;44m[mosh is suspended.]\033[m\n", stdout );

      fflush( NULL );

      /* actually suspend */
      kill( 0, SIGSTOP );

      resume();
    } else if ( ( the_byte == escape_pass_key ) || ( the_byte == escape_pass_key2 ) ) {
      /* Emulation sequence to type escape_key is escape_key +
         escape_pass_key (that is escape key without Ctrl) */
      net.get_current_state().push_back( Parser::UserByte( escape_key ) );
    } else {
      /* Escape key followed by anything other than . and ^ gets sent literally */
      net.get_current_state().push_back( Parser::UserByte( escape_key ) );
      net.get_current_state().push_back( Parser::UserByte( the_byte ) );
    }

    quit_sequence_started = false;

    if ( overlays.get_notification_engine().get_notification_string() == escape_key_help ) {
      overlays.get_notification_engine().set_notification_string( L"" );
    }

    return InputAction::CONTINUE;
  }

  quit_sequence_started = ( escape_key > 0 ) && ( the_byte == escape_key ) && ( lf_entered || ( !escape_requires_lf ) );
  if ( quit_sequence_started ) {
    lf_entered = false;
    overlays.get_notification_engine().set_notification_string( escape_key_help, true, false );
    return InputAction::CONTINUE;
  }

  lf_entered
    = ( ( the_byte == 0x0A ) || ( the_byte == 0x0D ) ); /* LineFeed, Ctrl-J, '\n' or CarriageReturn, Ctrl-M, '\r' */

  if ( the_byte == 0x0C ) { /* Ctrl-L */
    repaint_requested = true;
  }

  net.get_current_state().push_back( Parser::UserByte( the_byte ) );

  return InputAction::CONTINUE;
}

bool STMClient::apply_bytes_to_keystroke_stream( const std::string& bytes, bool paste )
{
  for ( size_t i = 0; i < bytes.size(); i++ ) {
    char the_byte = bytes[i];

    if ( !paste ) {
      overlays.get_prediction_engine().new_user_byte( the_byte, local_framebuffer );
    }

    InputAction action = process_input_byte( the_byte );
    if ( action == InputAction::STOP_EOF ) {
      return false;
    }
    if ( action == InputAction::STOP_OK ) {
      return true;
    }
  }

  return true;
}

bool STMClient::process_user_input( int fd )
{
  const int buf_size = 16384;
  char buf[buf_size];

  /* fill buffer if possible */
  ssize_t bytes_read = read( fd, buf, buf_size );
  if ( bytes_read == 0 ) { /* EOF */
    return false;
  } else if ( bytes_read < 0 ) {
    perror( "read" );
    return false;
  }

  NetworkType& net = *network;

  if ( net.shutdown_in_progress() ) {
    return true;
  }
  overlays.get_prediction_engine().set_local_frame_sent( net.get_sent_state_last() );

  /* Filter out our own theme-probe replies before they reach the keystroke stream. */
  std::string passthrough;
  reply_filter.feed( std::string( buf, bytes_read ), passthrough );

  std::vector<Terminal::TerminalReplyFilter::Reply> replies = reply_filter.take_replies();
  for ( std::vector<Terminal::TerminalReplyFilter::Reply>::const_iterator it = replies.begin(); it != replies.end();
        it++ ) {
    switch ( it->kind ) {
      case Terminal::TerminalReplyFilter::Reply::COLOR_SCHEME:
        if ( theme_scheme != 0 && theme_scheme != it->scheme ) {
          /* An actual switch: the default colours have probably changed too. */
          swrite( STDOUT_FILENO, THEME_COLOR_QUERY );
        }
        theme_scheme = it->scheme;
        break;
      case Terminal::TerminalReplyFilter::Reply::FOREGROUND:
        theme_fg = it->color;
        break;
      case Terminal::TerminalReplyFilter::Reply::BACKGROUND:
        theme_bg = it->color;
        break;
    }
  }
  if ( !replies.empty() && !net.shutdown_in_progress() ) {
    Parser::Theme current( theme_fg, theme_bg, theme_scheme );
    if ( !( current == last_sent_theme ) ) {
      net.get_current_state().push_back( current );
      last_sent_theme = current;
    }
  }

  pending_reply_deadline = reply_filter.has_pending() ? timestamp() + 100 : 0;

  /* Don't predict for bulk data. */
  bool paste = passthrough.size() > 100;
  if ( paste ) {
    overlays.get_prediction_engine().reset();
  }

  return apply_bytes_to_keystroke_stream( passthrough, paste );
}

bool STMClient::process_resize( void )
{
  /* get new size */
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    return false;
  }

  /* tell remote emulator */
  Parser::Resize res( window_size.ws_col, window_size.ws_row, window_size.ws_xpixel, window_size.ws_ypixel );

  if ( !network->shutdown_in_progress() ) {
    network->get_current_state().push_back( res );
  }

  /* note remote emulator will probably reply with its own Resize to adjust our state */

  /* tell prediction engine */
  overlays.get_prediction_engine().reset();

  return true;
}

bool STMClient::main( void )
{
  /* initialize signal handling and structures */
  main_init();

  /* Drop unnecessary privileges */
#ifdef HAVE_PLEDGE
  /* OpenBSD pledge() syscall */
  if ( pledge( "stdio inet tty", NULL ) ) {
    perror( "pledge() failed" );
    exit( 1 );
  }
#endif

  /* prepare to poll for events */
  Select& sel = Select::get_instance();

  while ( 1 ) {
    try {
      output_new_frame();

      int wait_time = std::min( network->wait_time(), overlays.wait_time() );

      /* Handle startup "Connecting..." message */
      if ( still_connecting() ) {
        wait_time = std::min( 250, wait_time );
      }

      /* Wake up in time to flush a held theme-probe reply that never completed */
      if ( pending_reply_deadline != 0 ) {
        uint64_t now = timestamp();
        int reply_wait = ( pending_reply_deadline > now ) ? static_cast<int>( pending_reply_deadline - now ) : 0;
        wait_time = std::min( wait_time, reply_wait );
      }

      /* poll for events */
      /* network->fd() can in theory change over time */
      sel.clear_fds();
      std::vector<int> fd_list( network->fds() );
      for ( std::vector<int>::const_iterator it = fd_list.begin(); it != fd_list.end(); it++ ) {
        sel.add_fd( *it );
      }
      sel.add_fd( STDIN_FILENO );

      int active_fds = sel.select( wait_time );
      if ( active_fds < 0 ) {
        perror( "select" );
        break;
      }

      bool network_ready_to_read = false;

      for ( std::vector<int>::const_iterator it = fd_list.begin(); it != fd_list.end(); it++ ) {
        if ( sel.read( *it ) ) {
          /* packet received from the network */
          /* we only read one socket each run */
          network_ready_to_read = true;
        }
      }

      if ( network_ready_to_read ) {
        process_network_input();
      }

      if ( sel.read( STDIN_FILENO )
           && !process_user_input( STDIN_FILENO ) ) { /* input from the user needs to be fed to the network */
        if ( !network->has_remote_addr() ) {
          break;
        } else if ( !network->shutdown_in_progress() ) {
          overlays.get_notification_engine().set_notification_string( std::wstring( L"Exiting..." ), true );
          network->start_shutdown();
        }
      }

      /* A held theme-probe reply that never completed within its deadline
         is just ordinary keystrokes -- release it. */
      if ( pending_reply_deadline != 0 && timestamp() >= pending_reply_deadline && reply_filter.has_pending() ) {
        std::string flushed;
        reply_filter.flush( flushed );
        pending_reply_deadline = 0;

        if ( !apply_bytes_to_keystroke_stream( flushed, false ) ) {
          if ( !network->has_remote_addr() ) {
            break;
          } else if ( !network->shutdown_in_progress() ) {
            overlays.get_notification_engine().set_notification_string( std::wstring( L"Exiting..." ), true );
            network->start_shutdown();
          }
        }
      }

      if ( sel.signal( SIGWINCH ) && !process_resize() ) { /* resize */
        return false;
      }

      if ( sel.signal( SIGCONT ) ) {
        resume();
      }

      if ( sel.signal( SIGTERM ) || sel.signal( SIGINT ) || sel.signal( SIGHUP ) || sel.signal( SIGPIPE ) ) {
        /* shutdown signal */
        if ( !network->has_remote_addr() ) {
          break;
        } else if ( !network->shutdown_in_progress() ) {
          overlays.get_notification_engine().set_notification_string(
            std::wstring( L"Signal received, shutting down..." ), true );
          network->start_shutdown();
        }
      }

      /* quit if our shutdown has been acknowledged */
      if ( network->shutdown_in_progress() && network->shutdown_acknowledged() ) {
        clean_shutdown = true;
        break;
      }

      /* quit after shutdown acknowledgement timeout */
      if ( network->shutdown_in_progress() && network->shutdown_ack_timed_out() ) {
        break;
      }

      /* quit if we received and acknowledged a shutdown request */
      if ( network->counterparty_shutdown_ack_sent() ) {
        clean_shutdown = true;
        break;
      }

      /* write diagnostic message if can't reach server */
      if ( still_connecting() && ( !network->shutdown_in_progress() )
           && ( timestamp() - network->get_latest_remote_state().timestamp > 250 ) ) {
        if ( timestamp() - network->get_latest_remote_state().timestamp > 15000 ) {
          if ( !network->shutdown_in_progress() ) {
            overlays.get_notification_engine().set_notification_string(
              std::wstring( L"Timed out waiting for server..." ), true );
            network->start_shutdown();
          }
        } else {
          overlays.get_notification_engine().set_notification_string( connecting_notification );
        }
      } else if ( ( network->get_remote_state_num() != 0 )
                  && ( overlays.get_notification_engine().get_notification_string() == connecting_notification ) ) {
        overlays.get_notification_engine().set_notification_string( L"" );
      }

      network->tick();

      std::string& send_error = network->get_send_error();
      if ( !send_error.empty() ) {
        overlays.get_notification_engine().set_network_error( send_error );
        send_error.clear();
      } else {
        overlays.get_notification_engine().clear_network_error();
      }
    } catch ( const Network::NetworkException& e ) {
      if ( !network->shutdown_in_progress() ) {
        overlays.get_notification_engine().set_network_error( e.what() );
      }

      struct timespec req;
      req.tv_sec = 0;
      req.tv_nsec = 200000000; /* 0.2 sec */
      nanosleep( &req, NULL );
      freeze_timestamp();
    } catch ( const Crypto::CryptoException& e ) {
      if ( e.fatal ) {
        throw;
      } else {
        wchar_t tmp[128];
        swprintf( tmp, 128, L"Crypto exception: %s", e.what() );
        overlays.get_notification_engine().set_notification_string( std::wstring( tmp ) );
      }
    }
  }
  return clean_shutdown;
}

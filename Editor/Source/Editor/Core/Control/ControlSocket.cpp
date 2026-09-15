#include <Common/Core/ErrnoText.hpp>
#include "ControlSocket.hpp"

#include <Common/Core/Logger.hpp>

#if defined( DESERT_PLATFORM_WINDOWS )
// NO WINDOWS IMPLEMENTATION, AND THAT IS A DECISION RATHER THAN AN OMISSION.
//
// AF_UNIX exists on Windows 10 1803 and later, so this could be written. It could not be RUN: the machine
// this engine is developed on is macOS, Windows reaches us only through CI, and CI does not run the editor
// at all. This project has already paid three times in one day for Windows code nobody could execute —
// `far` eaten by windef.h, path::native() being wide there, a float cancellation that only shows in
// Release — and each was found long after it was written.
//
// The control channel is a developer's tool. When somebody needs it on Windows they will have a Windows
// machine to prove it on, and the honest thing until then is to say so at the point of use. Everything
// above the transport — the protocol, the addressing, the ordering gate — is portable and is compiled and
// tested on every platform, so what is missing here is a socket and nothing else.
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Desert::Editor::Control
{
    namespace
    {
        /// The most a client may send before a newline arrives. A request is a short JSON object; a
        /// megabyte of it without one is a client that has lost the framing (or is not a client at all),
        /// and buffering without limit would let it consume the editor's memory one poll at a time.
        constexpr std::size_t kMaxRequestBytes = 1u << 20;
    } // namespace

    ControlSocket::~ControlSocket()
    {
        Close();
    }

    bool ControlSocket::SupportedOnThisPlatform() noexcept
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        return false;
#else
        return true;
#endif
    }

    bool ControlSocket::IsListening() const noexcept
    {
        return m_ListenFd >= 0;
    }

    bool ControlSocket::HasClient() const noexcept
    {
        return m_ClientFd >= 0;
    }

    uint64_t ControlSocket::ClientGeneration() const noexcept
    {
        return m_ClientGeneration;
    }

    bool ControlSocket::HasUnsentOutput() const noexcept
    {
        return !m_Outgoing.empty();
    }

#if defined( DESERT_PLATFORM_WINDOWS )

    Common::BoolResultStr ControlSocket::Listen( const std::string& path )
    {
        return Common::MakeFormattedError<bool>(
             "--control-socket '{}': the control channel has no transport on Windows. Its protocol, command "
             "addressing and frame ordering are built and tested here, but the socket itself is written and "
             "verified only where the editor runs, which today is macOS. This refuses rather than shipping "
             "an implementation nobody could execute.",
             path );
    }

    void ControlSocket::Close()
    {
    }

    void ControlSocket::DropClient()
    {
    }

    void ControlSocket::FlushOutput()
    {
    }

    void ControlSocket::DrainIncoming()
    {
    }

    std::optional<std::string> ControlSocket::PollRequestLine()
    {
        return std::nullopt;
    }

    void ControlSocket::ServiceConnection()
    {
    }

    void ControlSocket::SendResponseLine( const std::string& /*line*/ )
    {
    }

#else

    namespace
    {
        [[nodiscard]] bool MakeNonBlocking( int fd )
        {
            const int flags = ::fcntl( fd, F_GETFL, 0 );
            if ( flags < 0 )
                return false;
            return ::fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == 0;
        }

        /// Is something ALIVE on @p path? Asked by connecting to it, which is the only question whose
        /// answer is not a guess: a socket file outlives the process that made it, so "the file is there"
        /// says nothing at all. A live peer means another editor owns this path and we must not touch it;
        /// a refused connection means the file is a leftover and clearing it is safe.
        [[nodiscard]] bool SomethingIsListeningOn( const std::string& path )
        {
            const int probe = ::socket( AF_UNIX, SOCK_STREAM, 0 );
            if ( probe < 0 )
                return true; // cannot tell — assume the careful answer and refuse to clear the file

            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::strncpy( address.sun_path, path.c_str(), sizeof( address.sun_path ) - 1 );

            const bool connected =
                 ::connect( probe, reinterpret_cast<const sockaddr*>( &address ), sizeof( address ) ) == 0;
            ::close( probe );
            return connected;
        }
    } // namespace

    Common::BoolResultStr ControlSocket::Listen( const std::string& path )
    {
        if ( IsListening() )
            return Common::MakeError<bool>( "the control channel is already listening." );

        sockaddr_un address{};
        // The kernel's limit, not ours, and it is short (104 bytes on macOS). A path one byte too long
        // would be TRUNCATED by strncpy and bound as a DIFFERENT socket — the editor would report success
        // while listening somewhere the client never looks.
        if ( path.size() >= sizeof( address.sun_path ) )
        {
            return Common::MakeFormattedError<bool>(
                 "--control-socket '{}' is {} characters; a unix socket path may be at most {}. A longer one "
                 "would be silently truncated and bound somewhere else.",
                 path, path.size(), sizeof( address.sun_path ) - 1 );
        }

        if ( ::access( path.c_str(), F_OK ) == 0 )
        {
            if ( SomethingIsListeningOn( path ) )
            {
                return Common::MakeFormattedError<bool>(
                     "--control-socket '{}' is already served by another editor. Give this one a path of its "
                     "own; two editors sharing a socket would answer each other's clients.",
                     path );
            }

            if ( ::unlink( path.c_str() ) != 0 )
            {
                return Common::MakeFormattedError<bool>(
                     "--control-socket '{}' is a leftover from a dead editor and could not be removed: {}.", path,
                     Common::ErrnoText() );
            }
            LOG_INFO( "[Control] cleared a stale socket at '{}' (nothing was listening on it).", path );
        }

        const int fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
        if ( fd < 0 )
            return Common::MakeFormattedError<bool>( "--control-socket: socket() failed: {}",
                                                     Common::ErrnoText() );

        address.sun_family = AF_UNIX;
        std::strncpy( address.sun_path, path.c_str(), sizeof( address.sun_path ) - 1 );

        if ( ::bind( fd, reinterpret_cast<const sockaddr*>( &address ), sizeof( address ) ) != 0 )
        {
            const std::string reason = Common::ErrnoText();
            ::close( fd );
            return Common::MakeFormattedError<bool>( "--control-socket '{}': bind failed: {}", path, reason );
        }

        // OWNER ONLY. The default would be whatever the umask allows, and this socket runs commands in
        // somebody's editor — every command the palette offers, which includes saving over their scene.
        if ( ::chmod( path.c_str(), S_IRUSR | S_IWUSR ) != 0 )
        {
            const std::string reason = Common::ErrnoText();
            ::close( fd );
            ::unlink( path.c_str() );
            return Common::MakeFormattedError<bool>(
                 "--control-socket '{}': could not restrict the socket to its owner ({}); refusing to listen "
                 "on one anybody could drive.",
                 path, reason );
        }

        if ( ::listen( fd, 1 ) != 0 )
        {
            const std::string reason = Common::ErrnoText();
            ::close( fd );
            ::unlink( path.c_str() );
            return Common::MakeFormattedError<bool>( "--control-socket '{}': listen failed: {}", path, reason );
        }

        if ( !MakeNonBlocking( fd ) )
        {
            ::close( fd );
            ::unlink( path.c_str() );
            return Common::MakeFormattedError<bool>(
                 "--control-socket '{}': the listening socket could not be made non-blocking, and a blocking "
                 "accept in the frame loop would freeze the editor between clients.",
                 path );
        }

        m_ListenFd = fd;
        m_Path     = path;
        LOG_INFO( "[Control] listening on '{}'. Commands are the command palette's own; ask 'commands'.", path );
        return Common::MakeSuccess( true );
    }

    void ControlSocket::DropClient()
    {
        if ( m_ClientFd >= 0 )
        {
            ::close( m_ClientFd );
            m_ClientFd = -1;
        }
        m_Incoming.clear();
        m_Outgoing.clear();
    }

    void ControlSocket::Close()
    {
        DropClient();
        if ( m_ListenFd >= 0 )
        {
            ::close( m_ListenFd );
            m_ListenFd = -1;
        }
        if ( !m_Path.empty() )
        {
            ::unlink( m_Path.c_str() );
            m_Path.clear();
        }
    }

    void ControlSocket::FlushOutput()
    {
        while ( m_ClientFd >= 0 && !m_Outgoing.empty() )
        {
            // MSG_NOSIGNAL where it exists; on macOS the same is arranged with SO_NOSIGPIPE at accept.
            // Either way the point is that a client that walked away must not kill the editor with
            // SIGPIPE — losing an artist's session to a disconnected tool would be unforgivable.
#if defined( MSG_NOSIGNAL )
            const ssize_t sent = ::send( m_ClientFd, m_Outgoing.data(), m_Outgoing.size(), MSG_NOSIGNAL );
#else
            const ssize_t sent = ::send( m_ClientFd, m_Outgoing.data(), m_Outgoing.size(), 0 );
#endif
            if ( sent > 0 )
            {
                m_Outgoing.erase( 0, static_cast<std::size_t>( sent ) );
                continue;
            }

            if ( sent < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) )
                return; // the socket is full; try again next frame rather than stalling the editor

            LOG_WARN( "[Control] the client went away with {} bytes unsent; dropping it.", m_Outgoing.size() );
            DropClient();
            return;
        }
    }

    void ControlSocket::SendResponseLine( const std::string& line )
    {
        if ( m_ClientFd < 0 )
            return;

        m_Outgoing += line;
        m_Outgoing += '\n';
        FlushOutput();
    }

    void ControlSocket::DrainIncoming()
    {
        if ( m_ClientFd < 0 )
            return;

        char buffer[4096];
        for ( ;; )
        {
            const ssize_t received = ::recv( m_ClientFd, buffer, sizeof( buffer ), 0 );
            if ( received > 0 )
            {
                m_Incoming.append( buffer, static_cast<std::size_t>( received ) );
                if ( m_Incoming.size() > kMaxRequestBytes )
                {
                    LOG_WARN( "[Control] a client sent {} bytes with no newline; dropping it. Requests are "
                              "one line of JSON each.",
                              m_Incoming.size() );
                    DropClient();
                }
                continue;
            }

            if ( received == 0 )
            {
                LOG_INFO( "[Control] client disconnected." );
                DropClient();
                return;
            }

            if ( errno == EAGAIN || errno == EWOULDBLOCK )
                return;
            if ( errno == EINTR )
                continue;

            LOG_WARN( "[Control] read failed ({}); dropping the client.", Common::ErrnoText() );
            DropClient();
            return;
        }
    }

    void ControlSocket::ServiceConnection()
    {
        if ( m_ListenFd < 0 )
            return;

        // SPLIT OUT OF PollRequestLine BECAUSE THE EDITOR NOW HAS A REASON TO DO THIS AND NOT READ.
        //
        // A request can be parked for a whole boot while the editor comes up (FrameGate's readiness wait),
        // and during that time it must not read a SECOND request — one command in flight is the whole of
        // the ordering guarantee. But it must still notice that the asker has GONE, and a disconnected
        // peer is only detected by reading from it. Without this the editor would hold the channel for a
        // client that is no longer there, refusing every new one until the wait finished on its own.
        //
        // THE CLIENT WE HAVE IS SERVICED BEFORE A NEW ONE IS JUDGED, and the order is not cosmetic.
        //
        // A disconnected peer is only noticed by READING from it — recv returning zero is the whole of the
        // signal. With the accept loop first, a client that had already closed still occupied the slot at
        // the moment the next connection arrived, so the next connection was refused as "another client
        // already holds this channel". For the way this channel is actually used — one request per
        // invocation of desertctl, connect, ask, read, close — that made every second command fail. It was
        // not a race in the client; the editor simply had not looked yet.
        FlushOutput();
        DrainIncoming();

        // A waiting connection, if there is one and we have room for it.
        for ( ;; )
        {
            const int incoming = ::accept( m_ListenFd, nullptr, nullptr );
            if ( incoming < 0 )
                break;

            if ( m_ClientFd >= 0 )
            {
                // REFUSED IN WORDS rather than left hanging. A second client that simply never got an
                // answer would look exactly like an editor that had frozen.
                const std::string refusal =
                     R"({"id":0,"ok":false,"error":"another client already holds this editor's control )"
                     R"(channel; commands run one at a time on the editor thread."})"
                     "\n";
                (void)::send( incoming, refusal.data(), refusal.size(), 0 );
                ::close( incoming );
                continue;
            }

            if ( !MakeNonBlocking( incoming ) )
            {
                LOG_WARN( "[Control] a client could not be made non-blocking; refusing it." );
                ::close( incoming );
                continue;
            }

#if defined( SO_NOSIGPIPE )
            const int on = 1;
            (void)::setsockopt( incoming, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof( on ) );
#endif
            m_ClientFd = incoming;
            // A NEW CONNECTION IS A NEW GENERATION, and this is the line that lets a caller tell a
            // reconnect from a connection that never went away. Without it a request parked across a
            // disconnect-then-accept — which happens inside ONE service call, since the accept loop runs
            // right after the read that noticed the loss — would have its reply written to whoever took
            // the freed slot. Measured: a 311-command answer to client A, delivered to client B.
            ++m_ClientGeneration;
            m_Incoming.clear();
            m_Outgoing.clear();
            LOG_INFO( "[Control] client connected (generation {}).", m_ClientGeneration );
        }

        // A client accepted just above has not been read from yet. Reading here rather than waiting for
        // the next poll is what makes a request answerable on the frame it arrived on; without it every
        // command would cost an extra frame for no reason anybody could see.
        DrainIncoming();
    }

    std::optional<std::string> ControlSocket::PollRequestLine()
    {
        ServiceConnection();

        if ( m_ClientFd < 0 )
            return std::nullopt;

        const auto newline = m_Incoming.find( '\n' );
        if ( newline == std::string::npos )
            return std::nullopt;

        std::string line = m_Incoming.substr( 0, newline );
        m_Incoming.erase( 0, newline + 1 );
        if ( !line.empty() && line.back() == '\r' )
            line.pop_back();
        return line;
    }

#endif // DESERT_PLATFORM_WINDOWS
} // namespace Desert::Editor::Control

#include "ControlSocket.hpp"

#include <Common/Core/LocalSocket.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>

namespace Desert::Editor::Control
{
    /// Spelled short because it is on nearly every line below. See Common/Core/LocalSocket.hpp: it holds
    /// only what differs between the platforms, so the socket code here reads as ordinary socket code.
    namespace Socket = Common::LocalSocket;

    namespace
    {
        /// The most a client may send before a newline arrives. A request is a short JSON object; a
        /// megabyte of it without one is a client that has lost the framing (or is not a client at all),
        /// and buffering without limit would let it consume the editor's memory one poll at a time.
        constexpr std::size_t kMaxRequestBytes = 1u << 20;

        /// The most one send() is asked to take. Winsock's send counts bytes in an `int`, and a reply that
        /// did not fit in one would otherwise be handed over as a truncated length. The loop around it
        /// already deals with a partial send, so a chunk costs nothing but the cast it removes.
        constexpr std::size_t kMaxSendChunk = std::size_t{ 64 } * 1024;

        /// Is something ALIVE on @p path? Asked by connecting to it, which is the only question whose
        /// answer is not a guess: a socket file outlives the process that made it, so "the file is there"
        /// says nothing at all. A live peer means another editor owns this path and we must not touch it;
        /// a refused connection means the file is a leftover and clearing it is safe.
        [[nodiscard]] bool SomethingIsListeningOn( const std::string& path )
        {
            const Socket::Handle probe = Socket::Open();
            if ( probe < 0 )
                return true; // cannot tell — assume the careful answer and refuse to clear the file

            sockaddr_un address{};
            if ( !Socket::FillAddress( address, path ) )
                return true; // likewise: a path we cannot even form is not a path we may delete

            const bool connected =
                 ::connect( Socket::Raw( probe ), Socket::AsSockaddr( address ), sizeof( address ) ) == 0;
            Socket::Close( probe );
            return connected;
        }
    } // namespace

    ControlSocket::~ControlSocket()
    {
        Close();
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

    Common::BoolResultStr ControlSocket::Listen( const std::string& path )
    {
        if ( IsListening() )
            return Common::MakeError<bool>( "the control channel is already listening." );

        if ( const std::string unavailable = Socket::EnsureLibraryReady(); !unavailable.empty() )
        {
            return Common::MakeFormattedError<bool>( "--control-socket '{}': the platform's socket library "
                                                     "could not be started: {}.",
                                                     path, unavailable );
        }

        sockaddr_un address{};
        // The kernel's limit, not ours, and it is short (104 bytes on macOS, 108 on Windows and Linux). A
        // path one byte too long would be TRUNCATED and bound as a DIFFERENT socket — the editor would
        // report success while listening somewhere the client never looks.
        if ( !Socket::FillAddress( address, path ) )
        {
            return Common::MakeFormattedError<bool>(
                 "--control-socket '{}' is {} characters; a unix socket path may be at most {}. A longer one "
                 "would be silently truncated and bound somewhere else.",
                 path, path.size(), Socket::MaxPathLength() );
        }

        if ( Socket::FileExists( path ) )
        {
            if ( SomethingIsListeningOn( path ) )
            {
                return Common::MakeFormattedError<bool>(
                     "--control-socket '{}' is already served by another editor. Give this one a path of its "
                     "own; two editors sharing a socket would answer each other's clients.",
                     path );
            }

            if ( const std::string failure = Socket::RemoveFile( path ); !failure.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "--control-socket '{}' is a leftover from a dead editor and could not be removed: {}.", path,
                     failure );
            }
            LOG_INFO( "[Control] cleared a stale socket at '{}' (nothing was listening on it).", path );
        }

        const Socket::Handle fd = Socket::Open();
        if ( fd < 0 )
            return Common::MakeFormattedError<bool>( "--control-socket: socket() failed: {}",
                                                     Socket::LastErrorText() );

        if ( ::bind( Socket::Raw( fd ), Socket::AsSockaddr( address ), sizeof( address ) ) != 0 )
        {
            const std::string reason = Socket::LastErrorText();
            Socket::Close( fd );
            return Common::MakeFormattedError<bool>( "--control-socket '{}': bind failed: {}", path, reason );
        }

        // OWNER ONLY. The default would be whatever the umask allows (whatever the directory hands down, on
        // Windows), and this socket runs commands in somebody's editor — every command the palette offers,
        // which includes saving over their scene.
        if ( const std::string unprotected = Socket::RestrictToOwner( path ); !unprotected.empty() )
        {
            Socket::Close( fd );
            (void)Socket::RemoveFile( path );
            return Common::MakeFormattedError<bool>(
                 "--control-socket '{}': could not restrict the socket to its owner ({}); refusing to listen "
                 "on one anybody could drive.",
                 path, unprotected );
        }

        if ( ::listen( Socket::Raw( fd ), 1 ) != 0 )
        {
            const std::string reason = Socket::LastErrorText();
            Socket::Close( fd );
            (void)Socket::RemoveFile( path );
            return Common::MakeFormattedError<bool>( "--control-socket '{}': listen failed: {}", path, reason );
        }

        if ( !Socket::SetNonBlocking( fd ) )
        {
            const std::string reason = Socket::LastErrorText();
            Socket::Close( fd );
            (void)Socket::RemoveFile( path );
            return Common::MakeFormattedError<bool>(
                 "--control-socket '{}': the listening socket could not be made non-blocking ({}), and a "
                 "blocking accept in the frame loop would freeze the editor between clients.",
                 path, reason );
        }

        m_ListenFd = fd;
        m_Path     = path;
        LOG_INFO( "[Control] listening on '{}'. Commands are the command palette's own; ask 'commands'.", path );
        return Common::MakeSuccess( true );
    }

    void ControlSocket::DropClient() noexcept
    {
        if ( m_ClientFd >= 0 )
        {
            Socket::Close( m_ClientFd );
            m_ClientFd = Socket::kInvalid;
        }
        m_Incoming.clear();
        m_Outgoing.clear();
    }

    void ControlSocket::Close() noexcept
    {
        DropClient();
        if ( m_ListenFd >= 0 )
        {
            Socket::Close( m_ListenFd );
            m_ListenFd = Socket::kInvalid;
        }
        if ( !m_Path.empty() )
        {
            // NEITHER platform removes the socket file when the socket closes, so the next run would meet
            // its own leftover and have to probe it.
            (void)Socket::TryRemoveFile( m_Path );
            m_Path.clear();
        }
    }

    void ControlSocket::FlushOutput()
    {
        while ( m_ClientFd >= 0 && !m_Outgoing.empty() )
        {
            const auto chunk = static_cast<int>( std::min( m_Outgoing.size(), kMaxSendChunk ) );

            // MSG_NOSIGNAL where it exists; on macOS the same is arranged with SO_NOSIGPIPE at accept, and
            // on Windows there is no such signal to arrange for. Either way the point is that a client that
            // walked away must not kill the editor with SIGPIPE — losing an artist's session to a
            // disconnected tool would be unforgivable.
#if defined( MSG_NOSIGNAL )
            const auto sent = static_cast<std::ptrdiff_t>(
                 ::send( Socket::Raw( m_ClientFd ), m_Outgoing.data(), chunk, MSG_NOSIGNAL ) );
#else
            const auto sent =
                 static_cast<std::ptrdiff_t>( ::send( Socket::Raw( m_ClientFd ), m_Outgoing.data(), chunk, 0 ) );
#endif
            if ( sent > 0 )
            {
                m_Outgoing.erase( 0, static_cast<std::size_t>( sent ) );
                continue;
            }

            if ( sent < 0 && Socket::LastErrorIsWouldBlock() )
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
            const auto received = static_cast<std::ptrdiff_t>(
                 ::recv( Socket::Raw( m_ClientFd ), buffer, static_cast<int>( sizeof( buffer ) ), 0 ) );
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

            if ( Socket::LastErrorIsWouldBlock() )
                return;
            if ( Socket::LastErrorIsInterrupted() )
                continue;

            LOG_WARN( "[Control] read failed ({}); dropping the client.", Socket::LastErrorText() );
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
            const auto incoming =
                 static_cast<Socket::Handle>( ::accept( Socket::Raw( m_ListenFd ), nullptr, nullptr ) );
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
                (void)::send( Socket::Raw( incoming ), refusal.data(), static_cast<int>( refusal.size() ), 0 );
                Socket::Close( incoming );
                continue;
            }

            if ( !Socket::SetNonBlocking( incoming ) )
            {
                LOG_WARN( "[Control] a client could not be made non-blocking ({}); refusing it.",
                          Socket::LastErrorText() );
                Socket::Close( incoming );
                continue;
            }

#if defined( SO_NOSIGPIPE )
            const int on = 1;
            (void)::setsockopt( Socket::Raw( incoming ), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof( on ) );
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
} // namespace Desert::Editor::Control

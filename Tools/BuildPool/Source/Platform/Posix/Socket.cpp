// BuildPool platform, POSIX: the daemon's endpoint as a unix-domain stream socket. The server multiplexes every
// client with poll(); a lock file beside the socket makes the endpoint a single-instance lock without the race of
// two daemons removing each other's socket file.
#include "BuildPool/Platform.hpp"

#include "../Internal.hpp"
#include "Posix.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <unordered_map>

namespace buildpool
{
    namespace
    {
        constexpr uint32_t kSendTimeoutMs = 5000;

#ifdef MSG_NOSIGNAL
        constexpr int kSendFlags = MSG_NOSIGNAL;
#else
        constexpr int kSendFlags = 0; // macOS: SO_NOSIGPIPE is set on every socket instead
#endif

        void Prepare( int fd ) noexcept
        {
            fcntl( fd, F_SETFD, FD_CLOEXEC );
            fcntl( fd, F_SETFL, fcntl( fd, F_GETFL ) | O_NONBLOCK );
#ifdef SO_NOSIGPIPE
            int on = 1;
            setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof( on ) );
#endif
        }

        Result<sockaddr_un> AddressOf( const std::string& endpoint )
        {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if ( endpoint.size() >= sizeof( address.sun_path ) )
            {
                return Fail( "endpoint path " + endpoint + " is longer than sun_path (" +
                             std::to_string( sizeof( address.sun_path ) - 1 ) + " bytes)" );
            }
            std::memcpy( address.sun_path, endpoint.c_str(), endpoint.size() + 1 );
            return address;
        }

        bool IsGone( int error ) noexcept
        {
            return error == EPIPE || error == ECONNRESET || error == ENOTCONN;
        }

        // Writes all of `bytes` to a non-blocking socket, waiting for room at most `timeoutMs` in total.
        Status WriteAll( int fd, std::span<const std::byte> bytes, uint32_t timeoutMs, std::string_view peer )
        {
            const uint64_t deadline = MonotonicMs() + timeoutMs;
            while ( !bytes.empty() )
            {
                const ssize_t sent = ::send( fd, bytes.data(), bytes.size(), kSendFlags );
                if ( sent >= 0 )
                {
                    bytes = bytes.subspan( static_cast<size_t>( sent ) );
                    continue;
                }
                const int error = errno;
                if ( error == EINTR )
                {
                    continue;
                }
                if ( error != EAGAIN && error != EWOULDBLOCK )
                {
                    return Fail( posix::ErrnoText(
                         std::string( peer ) + ( IsGone( error ) ? " is gone" : " write" ), error ) );
                }
                const uint64_t now = MonotonicMs();
                if ( now >= deadline )
                {
                    return Fail( std::string( peer ) + " did not read a message for " +
                                 std::to_string( timeoutMs ) + " ms" );
                }
                pollfd entry{ fd, POLLOUT, 0 };
                ::poll( &entry, 1, static_cast<int>( deadline - now ) );
            }
            return Ok();
        }
    } // namespace

    // ---- Connection
    // ------------------------------------------------------------------------------------------------
    struct Connection::Impl
    {
        posix::Fd                      socket;
        detail::FrameReader            reader;
        std::array<std::byte, 1 << 16> buffer{};
    };

    Connection::Connection( std::unique_ptr<Impl> impl ) noexcept : m_impl( std::move( impl ) )
    {
    }
    Connection::Connection( Connection&& ) noexcept            = default;
    Connection& Connection::operator=( Connection&& ) noexcept = default;
    Connection::~Connection()                                  = default;

    Result<Connection> Connection::Connect( const std::string& endpoint, uint32_t timeoutMs ) noexcept
    {
        Result<sockaddr_un> address = AddressOf( endpoint );
        if ( !address )
        {
            return address.error();
        }
        const uint64_t deadline = MonotonicMs() + timeoutMs;
        auto           impl     = std::make_unique<Impl>();
        for ( ;; )
        {
            impl->socket.Reset( ::socket( AF_UNIX, SOCK_STREAM, 0 ) );
            if ( !impl->socket.Valid() )
            {
                return Fail( posix::ErrnoText( "socket", errno ) );
            }
            if ( ::connect( impl->socket.Get(), reinterpret_cast<const sockaddr*>( &address.value() ),
                            sizeof( sockaddr_un ) ) == 0 )
            {
                break;
            }
            const int error = errno;
            if ( error == ENOENT || error == ECONNREFUSED )
            {
                return Fail( "no daemon listens on " + endpoint );
            }
            if ( error != EAGAIN && error != EINTR )
            {
                return Fail( posix::ErrnoText( "connect " + endpoint, error ) );
            }
            // The daemon's backlog is full: it is alive and will accept.
            if ( MonotonicMs() >= deadline )
            {
                return Fail( "the daemon on " + endpoint + " accepted no connection within " +
                             std::to_string( timeoutMs ) + " ms" );
            }
            pollfd none{ -1, 0, 0 };
            ::poll( &none, 0, 10 );
        }
        Prepare( impl->socket.Get() );
        return Connection( std::move( impl ) );
    }

    Status Connection::Send( std::span<const std::byte> message ) noexcept
    {
        Result<std::vector<std::byte>> framed = detail::Frame( message );
        if ( !framed )
        {
            return framed.error();
        }
        return WriteAll( m_impl->socket.Get(), framed.value(), kSendTimeoutMs, "the daemon" );
    }

    Result<std::optional<std::vector<std::byte>>> Connection::Receive( uint32_t timeoutMs ) noexcept
    {
        const uint64_t deadline = MonotonicMs() + timeoutMs;
        for ( ;; )
        {
            Result<std::optional<std::vector<std::byte>>> next = m_impl->reader.Next();
            if ( !next || next.value().has_value() )
            {
                return next;
            }
            const ssize_t got = ::recv( m_impl->socket.Get(), m_impl->buffer.data(), m_impl->buffer.size(), 0 );
            if ( got > 0 )
            {
                m_impl->reader.Append(
                     std::span<const std::byte>( m_impl->buffer.data(), static_cast<size_t>( got ) ) );
                continue;
            }
            if ( got == 0 )
            {
                return Fail( "the daemon is gone (connection closed)" );
            }
            const int error = errno;
            if ( error == EINTR )
            {
                continue;
            }
            if ( error != EAGAIN && error != EWOULDBLOCK )
            {
                return Fail(
                     posix::ErrnoText( IsGone( error ) ? "the daemon is gone" : "read from the daemon", error ) );
            }
            const uint64_t now = MonotonicMs();
            if ( now >= deadline )
            {
                return std::optional<std::vector<std::byte>>();
            }
            pollfd entry{ m_impl->socket.Get(), POLLIN, 0 };
            ::poll( &entry, 1, static_cast<int>( deadline - now ) );
        }
    }

    // ---- Server
    // ----------------------------------------------------------------------------------------------------
    namespace
    {
        struct SocketClient
        {
            posix::Fd           socket;
            detail::FrameReader reader;
        };
    } // namespace

    struct Server::Impl
    {
        std::string                                path;
        posix::Fd                                  lock;
        posix::Fd                                  listener;
        ClientId                                   nextId = 1;
        std::unordered_map<ClientId, SocketClient> clients;
        std::vector<ServerEvent>                   pending;
        std::array<std::byte, 1 << 16>             buffer{};
        std::vector<pollfd>                        polled;
        std::vector<ClientId>                      polledIds;

        ~Impl()
        {
            if ( listener.Valid() )
            {
                ::unlink( path.c_str() ); // ours: the lock is still held here
            }
        }

        void Close( ClientId id )
        {
            if ( clients.erase( id ) > 0 )
            {
                pending.push_back( ServerEvent{ ServerEvent::Kind::Closed, id, {} } );
            }
        }

        void AcceptAll()
        {
            for ( ;; )
            {
                const int fd = ::accept( listener.Get(), nullptr, nullptr );
                if ( fd < 0 )
                {
                    if ( errno == EINTR || errno == ECONNABORTED )
                    {
                        continue;
                    }
                    return; // EAGAIN: no more waiting; any other error is retried on the next poll
                }
                Prepare( fd );
                const ClientId id = nextId++;
                clients[id].socket.Reset( fd );
                pending.push_back( ServerEvent{ ServerEvent::Kind::Connected, id, {} } );
            }
        }

        void ReadFrom( ClientId id )
        {
            auto found = clients.find( id );
            if ( found == clients.end() )
            {
                return;
            }
            SocketClient& client = found->second;
            for ( ;; )
            {
                const ssize_t got = ::recv( client.socket.Get(), buffer.data(), buffer.size(), 0 );
                if ( got > 0 )
                {
                    client.reader.Append(
                         std::span<const std::byte>( buffer.data(), static_cast<size_t>( got ) ) );
                    continue;
                }
                if ( got < 0 && errno == EINTR )
                {
                    continue;
                }
                if ( got < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) )
                {
                    break;
                }
                // 0 = the client closed; any other error = it is gone. Whole messages it sent first still count.
                Deliver( id, client );
                Close( id );
                return;
            }
            Deliver( id, client );
        }

        void Deliver( ClientId id, SocketClient& client )
        {
            for ( ;; )
            {
                Result<std::optional<std::vector<std::byte>>> next = client.reader.Next();
                if ( !next )
                {
                    Close( id ); // a broken frame: the stream cannot be trusted any more
                    return;
                }
                if ( !next.value().has_value() )
                {
                    return;
                }
                pending.push_back( ServerEvent{ ServerEvent::Kind::Message, id, std::move( *next.value() ) } );
            }
        }
    };

    Server::Server( std::unique_ptr<Impl> impl ) noexcept : m_impl( std::move( impl ) )
    {
    }
    Server::Server( Server&& ) noexcept            = default;
    Server& Server::operator=( Server&& ) noexcept = default;
    Server::~Server()                              = default;

    Result<Server> Server::Listen( const std::string& endpoint ) noexcept
    {
        Result<sockaddr_un> address = AddressOf( endpoint );
        if ( !address )
        {
            return address.error();
        }
        auto impl  = std::make_unique<Impl>();
        impl->path = endpoint;

        const std::string lockPath = endpoint + ".lock";
        impl->lock.Reset( ::open( lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600 ) );
        if ( !impl->lock.Valid() )
        {
            return Fail( posix::ErrnoText( "open " + lockPath, errno ) );
        }
        if ( ::flock( impl->lock.Get(), LOCK_EX | LOCK_NB ) != 0 )
        {
            if ( errno == EWOULDBLOCK )
            {
                return Fail( "already running: another daemon holds " + lockPath );
            }
            return Fail( posix::ErrnoText( "flock " + lockPath, errno ) );
        }
        // A socket file left behind by a daemon that died: probe it, then remove it.
        {
            posix::Fd probe( ::socket( AF_UNIX, SOCK_STREAM, 0 ) );
            if ( probe.Valid() && ::connect( probe.Get(), reinterpret_cast<const sockaddr*>( &address.value() ),
                                             sizeof( sockaddr_un ) ) == 0 )
            {
                return Fail( "already running: a daemon without the lock answers on " + endpoint );
            }
        }
        ::unlink( endpoint.c_str() );

        impl->listener.Reset( ::socket( AF_UNIX, SOCK_STREAM, 0 ) );
        if ( !impl->listener.Valid() )
        {
            return Fail( posix::ErrnoText( "socket", errno ) );
        }
        if ( ::bind( impl->listener.Get(), reinterpret_cast<const sockaddr*>( &address.value() ),
                     sizeof( sockaddr_un ) ) != 0 )
        {
            const std::string reason = posix::ErrnoText( "bind " + endpoint, errno );
            impl->listener.Reset();
            return Fail( reason );
        }
        ::chmod( endpoint.c_str(), 0600 ); // the pool of one user
        if ( ::listen( impl->listener.Get(), 256 ) != 0 )
        {
            return Fail( posix::ErrnoText( "listen " + endpoint, errno ) );
        }
        Prepare( impl->listener.Get() );
        return Server( std::move( impl ) );
    }

    Status Server::Poll( uint32_t timeoutMs, std::vector<ServerEvent>& events ) noexcept
    {
        Impl& impl = *m_impl;
        impl.polled.clear();
        impl.polledIds.clear();
        impl.polled.push_back( pollfd{ impl.listener.Get(), POLLIN, 0 } );
        impl.polledIds.push_back( 0 );
        for ( const auto& [id, client] : impl.clients )
        {
            impl.polled.push_back( pollfd{ client.socket.Get(), POLLIN, 0 } );
            impl.polledIds.push_back( id );
        }
        const int wait  = impl.pending.empty() ? static_cast<int>( timeoutMs ) : 0;
        const int ready = ::poll( impl.polled.data(), static_cast<nfds_t>( impl.polled.size() ), wait );
        if ( ready < 0 && errno != EINTR )
        {
            return Fail( posix::ErrnoText( "poll", errno ) );
        }
        if ( ready > 0 )
        {
            for ( size_t i = 1; i < impl.polled.size(); ++i )
            {
                if ( impl.polled[i].revents != 0 )
                {
                    impl.ReadFrom( impl.polledIds[i] );
                }
            }
            if ( impl.polled[0].revents & POLLIN )
            {
                impl.AcceptAll();
            }
        }
        for ( ServerEvent& event : impl.pending )
        {
            events.push_back( std::move( event ) );
        }
        impl.pending.clear();
        return Ok();
    }

    Status Server::Send( ClientId client, std::span<const std::byte> message ) noexcept
    {
        auto found = m_impl->clients.find( client );
        if ( found == m_impl->clients.end() )
        {
            return Fail( "client " + std::to_string( client ) + " is gone" );
        }
        Result<std::vector<std::byte>> framed = detail::Frame( message );
        if ( !framed )
        {
            return framed.error();
        }
        Status sent = WriteAll( found->second.socket.Get(), framed.value(), kSendTimeoutMs,
                                "client " + std::to_string( client ) );
        if ( !sent )
        {
            m_impl->Close( client ); // its Closed event follows from Poll
        }
        return sent;
    }

    void Server::Drop( ClientId client ) noexcept
    {
        m_impl->Close( client );
    }
} // namespace buildpool

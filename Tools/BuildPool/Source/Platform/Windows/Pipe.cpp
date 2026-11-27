// BuildPool platform, Windows: the daemon's endpoint as a named pipe. The server multiplexes every client on one
// I/O completion port (no 64-handle limit of WaitForMultipleObjects); the client side is one overlapped handle.
#include "BuildPool/Platform.hpp"

#include "../Internal.hpp"
#include "Win.hpp"

#include <array>
#include <unordered_map>

namespace buildpool
{
    namespace
    {
        constexpr DWORD    kPipeBufferBytes = 1 << 16;
        constexpr uint32_t kSendTimeoutMs   = 5000;

        // A write whose completion must not reach the completion port: the low bit of hEvent tells the kernel so.
        HANDLE PortlessEvent( HANDLE event ) noexcept
        {
            return reinterpret_cast<HANDLE>( reinterpret_cast<ULONG_PTR>( event ) | 1 );
        }

        bool IsGone( DWORD code ) noexcept
        {
            return code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA || code == ERROR_PIPE_NOT_CONNECTED;
        }

        // Writes all of `bytes` on an overlapped handle and waits for it, at most `timeoutMs`.
        Status WriteAll( HANDLE pipe, HANDLE event, std::span<const std::byte> bytes, uint32_t timeoutMs,
                         std::string_view peer )
        {
            while ( !bytes.empty() )
            {
                OVERLAPPED overlapped{};
                overlapped.hEvent = PortlessEvent( event );
                ResetEvent( event );
                DWORD written = 0;
                if ( !WriteFile( pipe, bytes.data(), static_cast<DWORD>( bytes.size() ), nullptr, &overlapped ) )
                {
                    const DWORD code = GetLastError();
                    if ( code != ERROR_IO_PENDING )
                    {
                        return Fail( win::ErrorText(
                             std::string( peer ) + ( IsGone( code ) ? " is gone" : " write" ), code ) );
                    }
                    if ( WaitForSingleObject( event, timeoutMs ) != WAIT_OBJECT_0 )
                    {
                        CancelIoEx( pipe, &overlapped );
                    }
                }
                if ( !GetOverlappedResult( pipe, &overlapped, &written, TRUE ) )
                {
                    const DWORD code = GetLastError();
                    if ( code == ERROR_OPERATION_ABORTED )
                    {
                        return Fail( std::string( peer ) + " did not read a message for " +
                                     std::to_string( timeoutMs ) + " ms" );
                    }
                    return Fail( win::ErrorText( std::string( peer ) + ( IsGone( code ) ? " is gone" : " write" ),
                                                 code ) );
                }
                bytes = bytes.subspan( written );
            }
            return Ok();
        }
    } // namespace

    // ---- Connection
    // ------------------------------------------------------------------------------------------------
    struct Connection::Impl
    {
        win::Handle                    pipe;
        win::Handle                    readEvent;
        win::Handle                    writeEvent;
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
        const std::wstring name     = win::Widen( endpoint );
        const uint64_t     deadline = MonotonicMs() + timeoutMs;
        auto               impl     = std::make_unique<Impl>();
        for ( ;; )
        {
            impl->pipe.Reset( CreateFileW( name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_OVERLAPPED, nullptr ) );
            if ( impl->pipe.Valid() )
            {
                break;
            }
            const DWORD code = GetLastError();
            if ( code == ERROR_FILE_NOT_FOUND )
            {
                return Fail( "no daemon listens on " + endpoint );
            }
            if ( code != ERROR_PIPE_BUSY )
            {
                return Fail( win::ErrorText( "connect " + endpoint, code ) );
            }
            const uint64_t now = MonotonicMs();
            if ( now >= deadline )
            {
                return Fail( "the daemon on " + endpoint + " accepted no connection within " +
                             std::to_string( timeoutMs ) + " ms" );
            }
            WaitNamedPipeW( name.c_str(), static_cast<DWORD>( deadline - now ) );
        }
        impl->readEvent.Reset( CreateEventW( nullptr, TRUE, FALSE, nullptr ) );
        impl->writeEvent.Reset( CreateEventW( nullptr, TRUE, FALSE, nullptr ) );
        if ( !impl->readEvent.Valid() || !impl->writeEvent.Valid() )
        {
            return Fail( win::LastErrorText( "CreateEventW" ) );
        }
        return Connection( std::move( impl ) );
    }

    Status Connection::Send( std::span<const std::byte> message ) noexcept
    {
        Result<std::vector<std::byte>> framed = detail::Frame( message );
        if ( !framed )
        {
            return framed.error();
        }
        // The daemon reads every connection on every poll; a write it does not take within the timeout means it
        // is wedged, which the client reports like a lost daemon.
        return WriteAll( m_impl->pipe.Get(), m_impl->writeEvent.Get(), framed.value(), kSendTimeoutMs,
                         "the daemon" );
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
            const uint64_t now = MonotonicMs();
            if ( now >= deadline && timeoutMs != 0 )
            {
                return std::optional<std::vector<std::byte>>();
            }
            const DWORD remaining = static_cast<DWORD>( deadline > now ? deadline - now : 0 );

            HANDLE     pipe = m_impl->pipe.Get();
            OVERLAPPED overlapped{};
            overlapped.hEvent = m_impl->readEvent.Get();
            ResetEvent( overlapped.hEvent );
            DWORD got = 0;
            if ( !ReadFile( pipe, m_impl->buffer.data(), static_cast<DWORD>( m_impl->buffer.size() ), nullptr,
                            &overlapped ) )
            {
                const DWORD code = GetLastError();
                if ( code != ERROR_IO_PENDING )
                {
                    return Fail(
                         win::ErrorText( IsGone( code ) ? "the daemon is gone" : "read from the daemon", code ) );
                }
                if ( WaitForSingleObject( overlapped.hEvent, remaining ) != WAIT_OBJECT_0 )
                {
                    CancelIoEx( pipe, &overlapped );
                }
            }
            if ( !GetOverlappedResult( pipe, &overlapped, &got, TRUE ) )
            {
                const DWORD code = GetLastError();
                if ( code == ERROR_OPERATION_ABORTED )
                {
                    return std::optional<std::vector<std::byte>>(); // the timeout passed with nothing read
                }
                return Fail(
                     win::ErrorText( IsGone( code ) ? "the daemon is gone" : "read from the daemon", code ) );
            }
            m_impl->reader.Append( std::span<const std::byte>( m_impl->buffer.data(), got ) );
            if ( timeoutMs == 0 )
            {
                // A zero timeout is a single look: whatever arrived is framed, nothing more is waited for.
                Result<std::optional<std::vector<std::byte>>> last = m_impl->reader.Next();
                return last;
            }
        }
    }

    // ---- Server
    // ----------------------------------------------------------------------------------------------------
    namespace
    {
        struct PipeClient
        {
            ClientId                       id = 0;
            win::Handle                    pipe;
            OVERLAPPED                     overlapped{};
            detail::FrameReader            reader;
            std::array<std::byte, 1 << 16> buffer{};
        };

        // Cancels the one read or connect a client may have outstanding and waits until the kernel is done with
        // its OVERLAPPED, so the client can be freed. The completion packet still reaches the port; Poll ignores
        // packets of ids it no longer knows.
        void Quiesce( PipeClient& client ) noexcept
        {
            if ( !client.pipe.Valid() )
            {
                return;
            }
            DWORD ignored = 0;
            if ( CancelIoEx( client.pipe.Get(), &client.overlapped ) || GetLastError() != ERROR_NOT_FOUND )
            {
                GetOverlappedResult( client.pipe.Get(), &client.overlapped, &ignored, TRUE );
            }
        }
    } // namespace

    struct Server::Impl
    {
        std::wstring                                              name;
        win::Handle                                               port;
        win::Handle                                               writeEvent;
        std::unique_ptr<PipeClient>                               listener;
        ClientId                                                  nextId = 1;
        std::unordered_map<ClientId, std::unique_ptr<PipeClient>> clients;
        std::vector<ServerEvent>                                  pending;

        ~Impl()
        {
            if ( listener )
            {
                Quiesce( *listener );
            }
            for ( auto& [id, client] : clients )
            {
                Quiesce( *client );
            }
        }

        // A new pipe instance waiting for the next client. `first` makes it the single-instance lock.
        Status Arm( bool first )
        {
            auto client = std::make_unique<PipeClient>();
            client->id  = nextId++;
            const DWORD openMode =
                 PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | ( first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0 );
            client->pipe.Reset(
                 CreateNamedPipeW( name.c_str(), openMode,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 0, nullptr ) );
            if ( !client->pipe.Valid() )
            {
                const DWORD code = GetLastError();
                if ( first && ( code == ERROR_ACCESS_DENIED || code == ERROR_PIPE_BUSY ) )
                {
                    return Fail( "already running: another daemon holds " + win::Narrow( name ) );
                }
                return Fail( win::ErrorText( "CreateNamedPipeW " + win::Narrow( name ), code ) );
            }
            if ( CreateIoCompletionPort( client->pipe.Get(), port.Get(), static_cast<ULONG_PTR>( client->id ),
                                         0 ) == nullptr )
            {
                return Fail( win::LastErrorText( "CreateIoCompletionPort (pipe)" ) );
            }
            if ( ConnectNamedPipe( client->pipe.Get(), &client->overlapped ) )
            {
                listener = std::move( client ); // completed at once: the packet is queued
                return Ok();
            }
            const DWORD code = GetLastError();
            if ( code == ERROR_IO_PENDING )
            {
                listener = std::move( client );
                return Ok();
            }
            if ( code == ERROR_PIPE_CONNECTED )
            {
                // A client connected between create and connect; no packet will come for it.
                listener = std::move( client );
                return Accept();
            }
            return Fail( win::ErrorText( "ConnectNamedPipe", code ) );
        }

        // The listener has a client: it becomes a connection and a new listener is armed.
        Status Accept()
        {
            std::unique_ptr<PipeClient> client = std::move( listener );
            const ClientId              id     = client->id;
            PipeClient&                 ref    = *client;
            clients.emplace( id, std::move( client ) );
            pending.push_back( ServerEvent{ ServerEvent::Kind::Connected, id, {} } );
            StartRead( ref );
            return Arm( false );
        }

        void StartRead( PipeClient& client )
        {
            client.overlapped = OVERLAPPED{};
            if ( ReadFile( client.pipe.Get(), client.buffer.data(), static_cast<DWORD>( client.buffer.size() ),
                           nullptr, &client.overlapped ) ||
                 GetLastError() == ERROR_IO_PENDING )
            {
                return; // the completion arrives through the port either way
            }
            Close( client.id );
        }

        // Forgets a client and reports it Closed exactly once.
        void Close( ClientId id )
        {
            auto found = clients.find( id );
            if ( found == clients.end() )
            {
                return;
            }
            Quiesce( *found->second );
            DisconnectNamedPipe( found->second->pipe.Get() );
            clients.erase( found );
            pending.push_back( ServerEvent{ ServerEvent::Kind::Closed, id, {} } );
        }

        Status Complete( const OVERLAPPED_ENTRY& entry )
        {
            const auto id = static_cast<ClientId>( entry.lpCompletionKey );
            if ( listener && id == listener->id && entry.lpOverlapped == &listener->overlapped )
            {
                DWORD ignored = 0;
                if ( GetOverlappedResult( listener->pipe.Get(), &listener->overlapped, &ignored, FALSE ) ||
                     GetLastError() == ERROR_PIPE_CONNECTED )
                {
                    return Accept();
                }
                // The would-be client left before it was accepted: offer a fresh instance.
                Quiesce( *listener );
                listener.reset();
                return Arm( false );
            }
            auto found = clients.find( id );
            if ( found == clients.end() || entry.lpOverlapped != &found->second->overlapped )
            {
                return Ok(); // a packet of a client already dropped
            }
            PipeClient& client = *found->second;
            DWORD       got    = 0;
            if ( !GetOverlappedResult( client.pipe.Get(), &client.overlapped, &got, FALSE ) )
            {
                Close( id );
                return Ok();
            }
            client.reader.Append( std::span<const std::byte>( client.buffer.data(), got ) );
            for ( ;; )
            {
                Result<std::optional<std::vector<std::byte>>> next = client.reader.Next();
                if ( !next )
                {
                    Close( id ); // a broken frame: the stream cannot be trusted any more
                    return Ok();
                }
                if ( !next.value().has_value() )
                {
                    break;
                }
                pending.push_back( ServerEvent{ ServerEvent::Kind::Message, id, std::move( *next.value() ) } );
            }
            StartRead( client );
            return Ok();
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
        auto impl  = std::make_unique<Impl>();
        impl->name = win::Widen( endpoint );
        impl->port.Reset( CreateIoCompletionPort( INVALID_HANDLE_VALUE, nullptr, 0, 1 ) );
        if ( !impl->port.Valid() )
        {
            return Fail( win::LastErrorText( "CreateIoCompletionPort" ) );
        }
        impl->writeEvent.Reset( CreateEventW( nullptr, TRUE, FALSE, nullptr ) );
        if ( !impl->writeEvent.Valid() )
        {
            return Fail( win::LastErrorText( "CreateEventW" ) );
        }
        if ( Status armed = impl->Arm( true ); !armed )
        {
            return armed.error();
        }
        return Server( std::move( impl ) );
    }

    Status Server::Poll( uint32_t timeoutMs, std::vector<ServerEvent>& events ) noexcept
    {
        Impl& impl = *m_impl;
        if ( !impl.listener )
        {
            if ( Status armed = impl.Arm( false ); !armed )
            {
                return armed.error();
            }
        }
        // Drain the port: wait only while there is nothing to report, then take whatever else is already queued.
        std::array<OVERLAPPED_ENTRY, 64> entries{};
        for ( int round = 0; round < 64; ++round )
        {
            const DWORD wait  = ( round == 0 && impl.pending.empty() ) ? timeoutMs : 0;
            ULONG       count = 0;
            if ( !GetQueuedCompletionStatusEx( impl.port.Get(), entries.data(),
                                               static_cast<ULONG>( entries.size() ), &count, wait, FALSE ) )
            {
                const DWORD code = GetLastError();
                if ( code == WAIT_TIMEOUT )
                {
                    break;
                }
                return Fail( win::ErrorText( "GetQueuedCompletionStatusEx", code ) );
            }
            Status failure = Ok();
            for ( ULONG i = 0; i < count; ++i )
            {
                if ( Status done = impl.Complete( entries[i] ); !done && failure )
                {
                    failure = done.error();
                }
            }
            if ( !failure )
            {
                return failure;
            }
            if ( count < entries.size() )
            {
                break;
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
        Status sent = WriteAll( found->second->pipe.Get(), m_impl->writeEvent.Get(), framed.value(),
                                kSendTimeoutMs, "client " + std::to_string( client ) );
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

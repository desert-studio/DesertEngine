#include "Net/FaultTransport.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <sstream>

namespace Desert::Net
{
    std::optional<std::uint8_t> MessageKindByName( std::string_view name )
    {
        for ( const MessageKindName& entry : kMessageKindNames )
            if ( entry.Name == name )
                return entry.Kind;
        return std::nullopt;
    }

    std::string MessageKindText( std::uint8_t kind )
    {
        for ( const MessageKindName& entry : kMessageKindNames )
            if ( entry.Kind == kind )
                return std::string( entry.Name );
        return std::format( "kind {}", kind );
    }

    namespace
    {
        // Every name of the table, '|'-separated, for a refusal that names what would have been accepted.
        std::string KindNamesText()
        {
            std::string names;
            for ( const MessageKindName& entry : kMessageKindNames )
            {
                if ( !names.empty() )
                    names += '|';
                names += entry.Name;
            }
            return names;
        }
    } // namespace

    FaultTransport::FaultTransport( INetTransport& inner, NetLogSink log )
         : m_Inner( inner ), m_Log( std::move( log ) )
    {
    }

    void FaultTransport::Arm( std::uint8_t kind, EMessageFault fault, std::uint32_t count )
    {
        const auto         armed = std::find_if( m_Faults.begin(), m_Faults.end(),
                                                 [kind]( const MessageFault& entry ) { return entry.Kind == kind; } );
        const MessageFault entry{ .Kind = kind, .Fault = fault, .Remaining = count, .Total = count };
        if ( armed != m_Faults.end() )
            *armed = entry;
        else
            m_Faults.push_back( entry );
        m_Log(
             std::format( "fault armed: {} {} x{}", MessageFaultName( fault ), MessageKindText( kind ), count ) );
    }

    bool FaultTransport::ArmFromConsole( std::string_view command, std::string& error )
    {
        std::istringstream words{ std::string( command ) };
        std::string        verb, kindName, countText, extra;
        words >> verb >> kindName >> countText >> extra;

        const auto refuse = [&]( std::string reason )
        {
            m_Log( std::format( "fault refused: {}", reason ) );
            error = std::move( reason );
            return false;
        };

        EMessageFault fault = EMessageFault::Drop;
        if ( verb == MessageFaultName( EMessageFault::Drop ) )
            fault = EMessageFault::Drop;
        else if ( verb == MessageFaultName( EMessageFault::Corrupt ) )
            fault = EMessageFault::Corrupt;
        else
            return refuse( std::format( "unknown verb '{}' (drop <Kind> [n] | corrupt <Kind> [n])", verb ) );

        const std::optional<std::uint8_t> kind = MessageKindByName( kindName );
        if ( !kind )
            return refuse( std::format( "{}: unknown kind '{}' ({})", verb, kindName, KindNamesText() ) );

        std::uint32_t count = 1;
        if ( !countText.empty() )
        {
            const char* const first  = countText.data();
            const char* const last   = first + countText.size();
            const auto        parsed = std::from_chars( first, last, count );
            if ( parsed.ec != std::errc() || parsed.ptr != last || count < 1 )
                return refuse(
                     std::format( "{} {}: count '{}' is not a whole number >= 1", verb, kindName, countText ) );
        }
        if ( !extra.empty() )
            return refuse( std::format( "{} {} {}: unexpected '{}'", verb, kindName, countText, extra ) );

        Arm( *kind, fault, count );
        return true;
    }

    FaultTransport::EVerdict FaultTransport::Apply( std::span<const std::byte> data, std::string_view way )
    {
        if ( data.empty() || m_Faults.empty() )
            return EVerdict::Pass;
        const auto kind  = static_cast<std::uint8_t>( data.front() );
        const auto armed = std::find_if( m_Faults.begin(), m_Faults.end(),
                                         [kind]( const MessageFault& entry ) { return entry.Kind == kind; } );
        if ( armed == m_Faults.end() )
            return EVerdict::Pass;

        --armed->Remaining;
        const std::uint32_t nth     = armed->Total - armed->Remaining;
        const std::string   name    = MessageKindText( kind );
        EVerdict            verdict = EVerdict::Drop;
        // A message too short to cut to two bytes and still be cut is dropped instead (the header's rule).
        if ( armed->Fault == EMessageFault::Corrupt && data.size() >= 3 )
        {
            verdict = EVerdict::Truncate;
            ++m_Corrupted;
            m_Log( std::format( "fault: corrupted {} #{} of {} ({} -> 2 B)", name, nth, armed->Total,
                                data.size() ) );
        }
        else
        {
            ++m_Dropped;
            m_Log( std::format( "fault: dropped {} #{} of {} ({}, {} B)", name, nth, armed->Total, way,
                                data.size() ) );
        }
        if ( armed->Remaining == 0 )
        {
            m_Log( std::format( "fault done: {} {} x{}", MessageFaultName( armed->Fault ), name, armed->Total ) );
            m_Faults.erase( armed );
        }
        return verdict;
    }

    bool FaultTransport::Listen( std::uint16_t port, std::string& error )
    {
        return m_Inner.Listen( port, error );
    }

    ConnectionId FaultTransport::Connect( std::string_view address, std::string& error )
    {
        return m_Inner.Connect( address, error );
    }

    ESendResult FaultTransport::Send( ConnectionId connection, std::span<const std::byte> data, ENetLane lane )
    {
        switch ( Apply( data, "Send" ) )
        {
            case EVerdict::Pass:
                return m_Inner.Send( connection, data, lane );
            case EVerdict::Truncate:
                return m_Inner.Send( connection, data.first( 2 ), lane );
            case EVerdict::Drop:
                break;
        }
        // Dropped: to the sender the message left, as one lost on the wire would have.
        return ESendResult::Sent;
    }

    void FaultTransport::Poll( INetTransportEvents& events )
    {
        Tap tap( *this, events );
        m_Inner.Poll( tap );
    }

    void FaultTransport::Close( ConnectionId connection, int code, std::string_view reason, bool flush )
    {
        m_Inner.Close( connection, code, reason, flush );
    }

    ConnectionStats FaultTransport::GetStats( ConnectionId connection ) const
    {
        return m_Inner.GetStats( connection );
    }

    std::string FaultTransport::DescribeConnection( ConnectionId connection ) const
    {
        return m_Inner.DescribeConnection( connection );
    }

    bool FaultTransport::SetSimulator( const NetSimulator& simulator, std::string& error )
    {
        return m_Inner.SetSimulator( simulator, error );
    }

    bool FaultTransport::SetSendRate( int minBytesPerSecond, int maxBytesPerSecond, std::string& error )
    {
        return m_Inner.SetSendRate( minBytesPerSecond, maxBytesPerSecond, error );
    }

    PeerIdentity FaultTransport::Peer( ConnectionId connection ) const
    {
        return m_Inner.Peer( connection );
    }

    bool FaultTransport::UseMeshCertificate( const std::string& caDirectory, std::string_view identity,
                                             std::string& error )
    {
        return m_Inner.UseMeshCertificate( caDirectory, identity, error );
    }

    void FaultTransport::Tap::OnConnected( ConnectionId connection, std::string_view address )
    {
        m_Events.OnConnected( connection, address );
    }

    void FaultTransport::Tap::OnClosed( ConnectionId connection, const DisconnectInfo& info )
    {
        m_Events.OnClosed( connection, info );
    }

    void FaultTransport::Tap::OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                                         NetClock::time_point received )
    {
        switch ( m_Owner.Apply( data, "Poll" ) )
        {
            case EVerdict::Pass:
                m_Events.OnMessage( connection, lane, data, received );
                return;
            case EVerdict::Truncate:
                m_Events.OnMessage( connection, lane, data.first( 2 ), received );
                return;
            case EVerdict::Drop:
                return;
        }
    }
} // namespace Desert::Net

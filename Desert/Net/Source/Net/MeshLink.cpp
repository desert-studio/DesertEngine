#include "Net/MeshLink.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <type_traits>

namespace Desert::Net
{
    namespace
    {
        // The mesh's own messages, above the client protocol's (Replication.hpp ends at kFirstSessionMessage +
        // 23).
        enum class EMesh : std::uint8_t
        {
            Hello = kFirstSessionMessage + 40, // u8 role, u32 index, u32 mesh port (0: listens for no peer)
            Directory,                         // u32 n x (u8 role, u32 index, str address)
            AuthorityMap,                      // u32 n x (str region, u32 sim, u32 epoch)
            Tick,                              // u32 step
            SimReady,
            LeaseRequest,
            Lease,         // u32 first, u32 count
            PawnWrite,     // pawn
            PawnRemove,    // u32 id
            StableWrite,   // u64 id, str component, str field, value             (sim -> store)
            StableChanged, // u64 id, str component, str field, u32 version, value (store -> subscriber)
            StableDestroy, // u64 id
            Synced,        // the store sent a new subscriber its full state
            PlayerJoined,  // u32 player, u64 account
            PlayerLeft,    // u32 player
            PlayerInput,   // u32 player, the client's Input message as received
            InputAck,      // u32 player, acked move (step, own movement state, queued steps)
            PawnSpawned,   // u32 owner, u32 player, pawn
            PawnDestroyed, // u32 id
        };
        constexpr NetId kLeaseSize = 1u << 16;

        class Writer
        {
        public:
            explicit Writer( EMesh type )
            {
                U8( static_cast<std::uint8_t>( type ) );
            }
            void U8( std::uint8_t v )
            {
                m_Out.push_back( static_cast<std::byte>( v ) );
            }
            void U32( std::uint32_t v )
            {
                for ( int i = 0; i < 4; ++i )
                    m_Out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
            }
            void U64( std::uint64_t v )
            {
                U32( static_cast<std::uint32_t>( v & 0xFFFFFFFFu ) );
                U32( static_cast<std::uint32_t>( v >> 32 ) );
            }
            void F32( float v )
            {
                U32( std::bit_cast<std::uint32_t>( v ) );
            }
            void Str( std::string_view text )
            {
                U32( static_cast<std::uint32_t>( text.size() ) );
                for ( const char c : text )
                    m_Out.push_back( static_cast<std::byte>( c ) );
            }
            void Bytes( std::span<const std::byte> bytes )
            {
                m_Out.insert( m_Out.end(), bytes.begin(), bytes.end() );
            }
            void Pawn( const PawnState& p )
            {
                U32( p.Id );
                F32( p.Position.X );
                F32( p.Position.Y );
                F32( p.Position.Z );
                F32( p.Yaw );
            }
            void Move( const ISimLink::AckedMove& m )
            {
                U32( m.Step );
                F32( m.State.Position.X );
                F32( m.State.Position.Y );
                F32( m.State.Position.Z );
                F32( m.State.Velocity.X );
                F32( m.State.Velocity.Y );
                F32( m.State.Velocity.Z );
                F32( m.State.Yaw );
                U8( m.State.Grounded ? 1 : 0 );
                U8( m.Buffered );
            }
            void Value( const ReplicatedValue& value )
            {
                U8( static_cast<std::uint8_t>( value.index() ) );
                std::visit(
                     [this]( const auto& v )
                     {
                         using T = std::decay_t<decltype( v )>;
                         if constexpr ( std::is_same_v<T, bool> )
                             U8( v ? 1 : 0 );
                         else if constexpr ( std::is_same_v<T, std::int32_t> )
                             U32( static_cast<std::uint32_t>( v ) );
                         else if constexpr ( std::is_same_v<T, float> )
                             F32( v );
                         else
                         {
                             for ( const std::int32_t mm : v.Millimetres )
                                 U32( static_cast<std::uint32_t>( mm ) );
                             U32( v.Rotation );
                         }
                     },
                     value );
            }
            std::vector<std::byte> Take()
            {
                return std::move( m_Out );
            }

        private:
            std::vector<std::byte> m_Out;
        };

        class Reader
        {
        public:
            explicit Reader( std::span<const std::byte> in ) : m_In( in )
            {
            }
            bool U8( std::uint8_t& v )
            {
                if ( m_At + 1 > m_In.size() )
                    return false;
                v = static_cast<std::uint8_t>( m_In[m_At++] );
                return true;
            }
            bool U32( std::uint32_t& v )
            {
                if ( m_At + 4 > m_In.size() )
                    return false;
                v = 0;
                for ( int i = 0; i < 4; ++i )
                    v |= static_cast<std::uint32_t>( m_In[m_At + i] ) << ( 8 * i );
                m_At += 4;
                return true;
            }
            bool U64( std::uint64_t& v )
            {
                std::uint32_t lo = 0, hi = 0;
                if ( !U32( lo ) || !U32( hi ) )
                    return false;
                v = lo | ( static_cast<std::uint64_t>( hi ) << 32 );
                return true;
            }
            bool F32( float& v )
            {
                std::uint32_t raw = 0;
                if ( !U32( raw ) )
                    return false;
                v = std::bit_cast<float>( raw );
                return true;
            }
            bool Str( std::string& text )
            {
                std::uint32_t size = 0;
                if ( !U32( size ) || m_At + size > m_In.size() )
                    return false;
                text.assign( reinterpret_cast<const char*>( m_In.data() + m_At ), size );
                m_At += size;
                return true;
            }
            bool Pawn( PawnState& p )
            {
                return U32( p.Id ) && F32( p.Position.X ) && F32( p.Position.Y ) && F32( p.Position.Z ) &&
                       F32( p.Yaw );
            }
            bool Move( ISimLink::AckedMove& m )
            {
                std::uint8_t grounded = 0;
                if ( !( U32( m.Step ) && F32( m.State.Position.X ) && F32( m.State.Position.Y ) &&
                        F32( m.State.Position.Z ) && F32( m.State.Velocity.X ) && F32( m.State.Velocity.Y ) &&
                        F32( m.State.Velocity.Z ) && F32( m.State.Yaw ) && U8( grounded ) && U8( m.Buffered ) ) )
                    return false;
                m.State.Grounded = grounded != 0;
                return true;
            }
            bool Value( ReplicatedValue& value )
            {
                std::uint8_t kind = 0;
                if ( !U8( kind ) )
                    return false;
                switch ( kind )
                {
                    case 0:
                    {
                        std::uint8_t b = 0;
                        if ( !U8( b ) || b > 1 )
                            return false;
                        value = b == 1;
                        return true;
                    }
                    case 1:
                    {
                        std::uint32_t i = 0;
                        if ( !U32( i ) )
                            return false;
                        value = static_cast<std::int32_t>( i );
                        return true;
                    }
                    case 2:
                    {
                        float f = 0.f;
                        if ( !F32( f ) )
                            return false;
                        value = f;
                        return true;
                    }
                    case 3:
                    {
                        QuantizedTransform t;
                        for ( std::int32_t& mm : t.Millimetres )
                        {
                            std::uint32_t raw = 0;
                            if ( !U32( raw ) )
                                return false;
                            mm = static_cast<std::int32_t>( raw );
                        }
                        if ( !U32( t.Rotation ) )
                            return false;
                        value = t;
                        return true;
                    }
                    default:
                        return false;
                }
            }
            [[nodiscard]] std::span<const std::byte> Rest() const
            {
                return m_In.subspan( m_At );
            }
            [[nodiscard]] bool Done() const
            {
                return m_At == m_In.size();
            }

        private:
            std::span<const std::byte> m_In;
            std::size_t                m_At = 0;
        };

        constexpr std::uint8_t Code( EMesh type )
        {
            return static_cast<std::uint8_t>( type );
        }

        std::vector<std::byte> Empty( EMesh type )
        {
            return Writer( type ).Take();
        }

        std::vector<std::byte> EncodeStableChanged( StableNetId id, const FieldUpdate& field )
        {
            Writer w( EMesh::StableChanged );
            w.U64( id );
            w.Str( field.Component );
            w.Str( field.Field );
            w.U32( field.Version );
            w.Value( field.Value );
            return w.Take();
        }

        std::vector<std::byte> EncodeU32( EMesh type, std::uint32_t value )
        {
            Writer w( type );
            w.U32( value );
            return w.Take();
        }

        std::vector<std::byte> EncodePawn( EMesh type, const PawnState& pawn )
        {
            Writer w( type );
            w.Pawn( pawn );
            return w.Take();
        }

        std::string HostOf( std::string_view address )
        {
            const std::size_t colon = address.rfind( ':' );
            return std::string( colon == std::string_view::npos ? address : address.substr( 0, colon ) );
        }
    } // namespace

    std::string_view MeshRoleName( EMeshRole role )
    {
        switch ( role )
        {
            case EMeshRole::Coordinator:
                return "coordinator";
            case EMeshRole::Store:
                return "store";
            case EMeshRole::Sim:
                return "sim";
            case EMeshRole::Gateway:
                return "gateway";
        }
        return "unknown";
    }

    std::optional<EMeshRole> MeshRoleFromName( std::string_view name )
    {
        for ( const EMeshRole role :
              { EMeshRole::Coordinator, EMeshRole::Store, EMeshRole::Sim, EMeshRole::Gateway } )
            if ( MeshRoleName( role ) == name )
                return role;
        return std::nullopt;
    }

    std::string MeshIdentity( EMeshRole role, std::uint32_t index )
    {
        return std::format( "mesh-{}-{}", MeshRoleName( role ), index );
    }

    std::optional<std::uint32_t> DecodeMeshTick( std::uint8_t type, std::span<const std::byte> body )
    {
        std::uint32_t step = 0;
        Reader        r( body );
        if ( type != Code( EMesh::Tick ) || !r.U32( step ) )
            return std::nullopt;
        return step;
    }

    std::vector<std::byte> EncodeMeshSimReady()
    {
        return Empty( EMesh::SimReady );
    }

    std::optional<std::vector<MeshPeerAddress>> DecodeMeshDirectory( std::uint8_t               type,
                                                                     std::span<const std::byte> body )
    {
        if ( type != Code( EMesh::Directory ) )
            return std::nullopt;
        Reader                       r( body );
        std::uint32_t                n = 0;
        std::vector<MeshPeerAddress> peers;
        if ( !r.U32( n ) )
            return std::nullopt;
        for ( std::uint32_t i = 0; i < n; ++i )
        {
            MeshPeerAddress peer;
            std::uint8_t    role = 0;
            if ( !r.U8( role ) || role > static_cast<std::uint8_t>( EMeshRole::Gateway ) || !r.U32( peer.Index ) ||
                 !r.Str( peer.Address ) )
                return std::nullopt;
            peer.Role = static_cast<EMeshRole>( role );
            peers.push_back( std::move( peer ) );
        }
        return peers;
    }

    // ---------------------------------------------------------------------------------------------- MeshEndpoint

    MeshEndpoint::MeshEndpoint( INetTransport& transport, EMeshRole role, std::uint32_t index,
                                std::uint16_t meshPort, NetLogSink log )
         : m_Transport( transport ), m_Role( role ), m_Index( index ), m_MeshPort( meshPort ),
           m_Log( std::move( log ) )
    {
    }

    bool MeshEndpoint::ConnectTo( EMeshRole role, std::uint32_t index, std::string_view address,
                                  std::string& error )
    {
        const ConnectionId connection = m_Transport.Connect( address, error );
        if ( connection == kInvalidConnection )
            return false;
        m_Links[connection] = Link{ true, false, role, index, std::string( address ) };
        m_Log( std::format( "mesh: connecting to {} {} at {}", MeshRoleName( role ), index, address ) );
        return true;
    }

    std::optional<ConnectionId> MeshEndpoint::PeerConnection( EMeshRole role, std::uint32_t index ) const
    {
        for ( const auto& [connection, link] : m_Links )
            if ( link.Up && link.Role == role && link.Index == index )
                return connection;
        return std::nullopt;
    }

    void MeshEndpoint::Send( ConnectionId connection, const std::vector<std::byte>& message )
    {
        if ( m_Transport.Send( connection, message, ENetLane::Rpc ) != ESendResult::Sent )
            m_Log( std::format( "mesh: send of message {} on connection {} failed", static_cast<int>( message[0] ),
                                connection ) );
    }

    void MeshEndpoint::SendToRole( EMeshRole role, const std::vector<std::byte>& message )
    {
        for ( const auto& [connection, link] : m_Links )
            if ( link.Up && link.Role == role )
                Send( connection, message );
    }

    std::vector<std::uint32_t> MeshEndpoint::PeersOfRole( EMeshRole role ) const
    {
        std::vector<std::uint32_t> out;
        for ( const auto& [connection, link] : m_Links )
            if ( link.Up && link.Role == role )
                out.push_back( link.Index );
        return out;
    }

    std::string MeshEndpoint::AddressOf( ConnectionId connection ) const
    {
        const auto it = m_Links.find( connection );
        return it == m_Links.end() ? std::string() : it->second.Address;
    }

    bool MeshEndpoint::Authenticate( ConnectionId connection, EMeshRole role, std::uint32_t index )
    {
        const PeerIdentity peer     = m_Transport.Peer( connection );
        const std::string  expected = "str:" + MeshIdentity( role, index );
        if ( !peer.Authenticated || peer.Identity != expected )
        {
            m_Log( std::format( "mesh: link {} refused - peer '{}' {}, expected {} signed by the mesh CA",
                                connection, peer.Identity,
                                peer.Authenticated ? "is authenticated" : "has no CA-signed certificate",
                                expected ) );
            m_Transport.Close( connection, kAppCloseCodeMin + 100, "mesh certificate refused", false );
            m_Links.erase( connection );
            return false;
        }
        m_Log( std::format( "mesh: link to {} {} up - certificate '{}' signed by the mesh CA",
                            MeshRoleName( role ), index, peer.Identity ) );
        return true;
    }

    void MeshEndpoint::OnConnected( ConnectionId connection, std::string_view address )
    {
        const auto it = m_Links.find( connection );
        if ( it != m_Links.end() && it->second.Outgoing )
        {
            const Link link = it->second;
            if ( !Authenticate( connection, link.Role, link.Index ) )
                return;
            Writer hello( EMesh::Hello );
            hello.U8( static_cast<std::uint8_t>( m_Role ) );
            hello.U32( m_Index );
            hello.U32( m_MeshPort );
            Send( connection, hello.Take() );
            m_Links[connection].Up = true;
            if ( m_Handler.PeerUp )
                m_Handler.PeerUp( link.Role, link.Index, connection, 0 );
            return;
        }
        if ( m_Role == EMeshRole::Gateway )
        {
            m_ClientConnections.insert( connection );
            if ( m_Clients )
                m_Clients->OnConnected( connection, address );
            return;
        }
        m_Links[connection] = Link{ false, false, EMeshRole::Sim, 0, std::string( address ) };
    }

    void MeshEndpoint::OnClosed( ConnectionId connection, const DisconnectInfo& info )
    {
        if ( m_ClientConnections.erase( connection ) )
        {
            if ( m_Clients )
                m_Clients->OnClosed( connection, info );
            return;
        }
        const auto it = m_Links.find( connection );
        if ( it == m_Links.end() )
            return;
        const Link link = it->second;
        m_Links.erase( it );
        m_Log( std::format( "mesh: link {} ({} {}) closed: {} {}", connection, MeshRoleName( link.Role ),
                            link.Index, info.Code, info.Reason ) );
        if ( link.Up && m_Handler.PeerLost )
            m_Handler.PeerLost( link.Role, link.Index, connection );
    }

    void MeshEndpoint::OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data )
    {
        if ( m_ClientConnections.contains( connection ) )
        {
            if ( m_Clients )
                m_Clients->OnMessage( connection, lane, data );
            return;
        }
        const auto it = m_Links.find( connection );
        if ( it == m_Links.end() || data.empty() )
            return;
        const std::uint8_t               type = static_cast<std::uint8_t>( data[0] );
        const std::span<const std::byte> body = data.subspan( 1 );
        if ( !it->second.Up )
        {
            Reader        r( body );
            std::uint8_t  role  = 0;
            std::uint32_t index = 0, port = 0;
            if ( type != Code( EMesh::Hello ) || !r.U8( role ) ||
                 role > static_cast<std::uint8_t>( EMeshRole::Gateway ) || !r.U32( index ) || !r.U32( port ) ||
                 port > 65535 )
            {
                m_Log( std::format( "mesh: link {} sent message {} before a valid Hello - closed", connection,
                                    type ) );
                m_Transport.Close( connection, kAppCloseCodeMin + 101, "mesh hello expected", false );
                m_Links.erase( it );
                return;
            }
            if ( !Authenticate( connection, static_cast<EMeshRole>( role ), index ) )
                return;
            Link& link = m_Links[connection];
            link.Up    = true;
            link.Role  = static_cast<EMeshRole>( role );
            link.Index = index;
            if ( m_Handler.PeerUp )
                m_Handler.PeerUp( link.Role, index, connection, static_cast<std::uint16_t>( port ) );
            return;
        }
        if ( m_Handler.Message )
            m_Handler.Message( it->second.Role, it->second.Index, connection, type, body );
    }

    // ------------------------------------------------------------------------------------------- MeshCoordinator

    MeshCoordinator::MeshCoordinator( MeshEndpoint& endpoint, NetLogSink log )
         : m_Endpoint( endpoint ), m_Log( std::move( log ) )
    {
        m_Endpoint.SetHandler(
             { [this]( EMeshRole role, std::uint32_t index, ConnectionId connection, std::uint16_t port )
               { PeerUp( role, index, connection, port ); },
               [this]( EMeshRole role, std::uint32_t index, ConnectionId ) { PeerLost( role, index ); },
               [this]( EMeshRole role, std::uint32_t index, ConnectionId, std::uint8_t type,
                       std::span<const std::byte> body ) { Message( role, index, type, body ); } } );
    }

    void MeshCoordinator::PeerUp( EMeshRole role, std::uint32_t index, ConnectionId connection,
                                  std::uint16_t port )
    {
        MeshPeerAddress peer{
             role, index, port ? std::format( "{}:{}", HostOf( m_Endpoint.AddressOf( connection ) ), port ) : "" };
        m_Log( std::format( "coordinator: {} {} joined the mesh{}", MeshRoleName( role ), index,
                            peer.Address.empty() ? std::string() : " (mesh address " + peer.Address + ")" ) );
        m_Peers[{ role, index }] = std::move( peer );
        Publish();
    }

    void MeshCoordinator::PeerLost( EMeshRole role, std::uint32_t index )
    {
        m_Peers.erase( { role, index } );
        if ( role == EMeshRole::Sim )
            m_ReadySims.erase( index );
        m_Log( std::format( "coordinator: {} {} left the mesh", MeshRoleName( role ), index ) );
        Publish();
    }

    void MeshCoordinator::Message( EMeshRole role, std::uint32_t index, std::uint8_t type,
                                   std::span<const std::byte> )
    {
        if ( type == Code( EMesh::SimReady ) && role == EMeshRole::Sim )
        {
            m_ReadySims.insert( index );
            m_Log( std::format( "coordinator: sim {} is ready (holds its NetId lease)", index ) );
            Publish();
        }
    }

    void MeshCoordinator::Publish()
    {
        // THE AUTHORITY MAP: one region, the whole world, owned by the lowest ready sim. Its epoch moves on every
        // change of owner (the handover of §2M.4 starts from here in NET-MESH-2).
        std::vector<MeshRegion> regions;
        if ( !m_ReadySims.empty() )
        {
            const std::uint32_t sim   = *m_ReadySims.begin();
            std::uint32_t       epoch = m_Regions.empty() ? 1 : m_Regions.front().Epoch;
            if ( !m_Regions.empty() && m_Regions.front().Sim != sim )
                ++epoch;
            regions.push_back( { "world", sim, epoch } );
        }
        const bool mapChanged = regions.size() != m_Regions.size() ||
                                ( !regions.empty() && ( regions.front().Sim != m_Regions.front().Sim ||
                                                        regions.front().Epoch != m_Regions.front().Epoch ) );
        m_Regions = std::move( regions );
        if ( mapChanged )
            m_Log( m_Regions.empty()
                        ? std::string( "coordinator: authority map empty (no ready sim)" )
                        : std::format( "coordinator: authority map: region '{}' -> sim {} (epoch {})",
                                       m_Regions.front().Name, m_Regions.front().Sim, m_Regions.front().Epoch ) );

        Writer                              directory( EMesh::Directory );
        std::vector<const MeshPeerAddress*> listed;
        for ( const auto& [key, peer] : m_Peers )
            if ( peer.Role == EMeshRole::Store || ( peer.Role == EMeshRole::Sim && !peer.Address.empty() ) )
                listed.push_back( &peer );
        directory.U32( static_cast<std::uint32_t>( listed.size() ) );
        for ( const MeshPeerAddress* peer : listed )
        {
            directory.U8( static_cast<std::uint8_t>( peer->Role ) );
            directory.U32( peer->Index );
            directory.Str( peer->Address );
        }
        const std::vector<std::byte> directoryMessage = directory.Take();
        Writer                       map( EMesh::AuthorityMap );
        map.U32( static_cast<std::uint32_t>( m_Regions.size() ) );
        for ( const MeshRegion& region : m_Regions )
        {
            map.Str( region.Name );
            map.U32( region.Sim );
            map.U32( region.Epoch );
        }
        const std::vector<std::byte> mapMessage = map.Take();
        for ( const EMeshRole role : { EMeshRole::Store, EMeshRole::Sim, EMeshRole::Gateway } )
        {
            m_Endpoint.SendToRole( role, directoryMessage );
            m_Endpoint.SendToRole( role, mapMessage );
        }
    }

    void MeshCoordinator::Tick( std::uint32_t step )
    {
        const std::vector<std::byte> tick = EncodeU32( EMesh::Tick, step );
        for ( const EMeshRole role : { EMeshRole::Store, EMeshRole::Sim, EMeshRole::Gateway } )
            m_Endpoint.SendToRole( role, tick );
    }

    std::string MeshCoordinator::Describe() const
    {
        std::string text;
        for ( const auto& [key, peer] : m_Peers )
            text += std::format( "{}{} {}", text.empty() ? "" : ", ", MeshRoleName( peer.Role ), peer.Index );
        return std::format( "{} peer(s) [{}], {} ready sim(s), {} region(s)", m_Peers.size(), text,
                            m_ReadySims.size(), m_Regions.size() );
    }

    // ------------------------------------------------------------------------------------------------- MeshStore

    MeshStore::MeshStore( MeshEndpoint& endpoint, NetLogSink log )
         : m_Endpoint( endpoint ), m_Log( std::move( log ) )
    {
        m_Endpoint.SetHandler(
             { [this]( EMeshRole role, std::uint32_t index, ConnectionId connection, std::uint16_t )
               { PeerUp( role, index, connection ); }, [this]( EMeshRole role, std::uint32_t index, ConnectionId )
               { m_Log( std::format( "store: {} {} unsubscribed", MeshRoleName( role ), index ) ); },
               [this]( EMeshRole role, std::uint32_t index, ConnectionId, std::uint8_t type,
                       std::span<const std::byte> body ) { Message( role, index, type, body ); } } );
    }

    void MeshStore::PeerUp( EMeshRole role, std::uint32_t index, ConnectionId connection )
    {
        if ( role != EMeshRole::Sim && role != EMeshRole::Gateway )
            return;
        // THE FULL STATE first (a late subscriber starts from the state of record), then the change stream.
        std::size_t fields = 0;
        if ( role == EMeshRole::Gateway )
            for ( const PawnState& pawn : m_State.All() )
                m_Endpoint.Send( connection, EncodePawn( EMesh::PawnWrite, pawn ) );
        for ( const auto& [id, objectFields] : m_State.StableObjects() )
            for ( const FieldUpdate& field : objectFields )
            {
                m_Endpoint.Send( connection, EncodeStableChanged( id, field ) );
                ++fields;
            }
        for ( const StableNetId id : m_State.DestroyedStable() )
        {
            Writer w( EMesh::StableDestroy );
            w.U64( id );
            m_Endpoint.Send( connection, w.Take() );
        }
        m_Endpoint.Send( connection, Empty( EMesh::Synced ) );
        m_Log( std::format( "store: {} {} subscribed - sent the full state: {} pawn(s), {} stable field(s), {} "
                            "destroyed stable id(s)",
                            MeshRoleName( role ), index, role == EMeshRole::Gateway ? m_State.All().size() : 0,
                            fields, m_State.DestroyedStable().size() ) );
    }

    void MeshStore::Broadcast( const std::vector<std::byte>& message )
    {
        m_Endpoint.SendToRole( EMeshRole::Gateway, message );
        m_Endpoint.SendToRole( EMeshRole::Sim, message );
    }

    void MeshStore::Message( EMeshRole role, std::uint32_t index, std::uint8_t type,
                             std::span<const std::byte> body )
    {
        Reader r( body );
        if ( type == Code( EMesh::AuthorityMap ) )
        {
            std::uint32_t n = 0;
            if ( !r.U32( n ) )
                return;
            m_Regions.clear();
            for ( std::uint32_t i = 0; i < n; ++i )
            {
                MeshRegion region;
                if ( !r.Str( region.Name ) || !r.U32( region.Sim ) || !r.U32( region.Epoch ) )
                    return;
                m_Log( std::format( "store: authority map from the coordinator: region '{}' -> sim {} (epoch {})",
                                    region.Name, region.Sim, region.Epoch ) );
                m_Regions.push_back( std::move( region ) );
            }
            return;
        }
        if ( role != EMeshRole::Sim )
            return; // only sims write; a gateway's writes would be a protocol error, the directory/tick are info
        if ( type == Code( EMesh::LeaseRequest ) )
        {
            Writer w( EMesh::Lease );
            w.U32( m_NextLease );
            w.U32( kLeaseSize );
            if ( const auto connection = m_Endpoint.PeerConnection( EMeshRole::Sim, index ) )
                m_Endpoint.Send( *connection, w.Take() );
            m_Log( std::format( "store: NetId block [{}, {}) leased to sim {}", m_NextLease,
                                m_NextLease + kLeaseSize, index ) );
            m_NextLease += kLeaseSize;
        }
        else if ( type == Code( EMesh::PawnWrite ) )
        {
            PawnState pawn;
            if ( !r.Pawn( pawn ) )
                return;
            if ( m_PawnAuthority.emplace( pawn.Id, index ).second )
                m_Log( std::format( "store: pawn net id {} - authority sim {} (epoch {})", pawn.Id, index,
                                    m_Regions.empty() ? 0u : m_Regions.front().Epoch ) );
            m_State.Write( pawn );
            m_Endpoint.SendToRole( EMeshRole::Gateway, EncodePawn( EMesh::PawnWrite, pawn ) );
        }
        else if ( type == Code( EMesh::PawnRemove ) )
        {
            std::uint32_t id = 0;
            if ( !r.U32( id ) )
                return;
            m_State.Remove( id );
            m_PawnAuthority.erase( id );
            m_Endpoint.SendToRole( EMeshRole::Gateway, EncodeU32( EMesh::PawnRemove, id ) );
        }
        else if ( type == Code( EMesh::StableWrite ) )
        {
            StableNetId     id = 0;
            std::string     component, field;
            ReplicatedValue value;
            if ( !r.U64( id ) || !r.Str( component ) || !r.Str( field ) || !r.Value( value ) )
                return;
            const std::uint32_t version = m_State.WriteStable( id, component, field, value );
            const auto&         fields  = m_State.StableObjects().at( id );
            const auto          it      = std::find_if( fields.begin(), fields.end(), [&]( const FieldUpdate& f )
                                                        { return f.Component == component && f.Field == field; } );
            Broadcast( EncodeStableChanged( id, *it ) );
            const std::vector<std::uint32_t> gateways = m_Endpoint.PeersOfRole( EMeshRole::Gateway );
            std::string                      names;
            for ( const std::uint32_t g : gateways )
                names += std::format( "{}gateway {}", names.empty() ? "" : ", ", g );
            m_Log( std::format( "store: stable {} {}.{} = {} (version {}) written by sim {} -> {} as state", id,
                                component, field, ReplicatedValueText( value ), version, index,
                                names.empty() ? std::string( "no gateway yet" ) : names ) );
        }
        else if ( type == Code( EMesh::StableDestroy ) )
        {
            StableNetId id = 0;
            if ( !r.U64( id ) )
                return;
            if ( m_State.DestroyStable( id ) )
            {
                Writer w( EMesh::StableDestroy );
                w.U64( id );
                Broadcast( w.Take() );
                m_Log(
                     std::format( "store: stable {} destroyed by sim {} - in the world delta table", id, index ) );
            }
        }
    }

    // ------------------------------------------------------------------------------------- RemoteReplicationStore

    RemoteReplicationStore::RemoteReplicationStore( MeshEndpoint& endpoint, NetLogSink log )
         : m_Endpoint( endpoint ), m_Log( std::move( log ) )
    {
    }

    void RemoteReplicationStore::RequestLease()
    {
        if ( m_LeaseAsked )
            return;
        m_LeaseAsked = true;
        m_Endpoint.SendToRole( EMeshRole::Store, Empty( EMesh::LeaseRequest ) );
    }

    NetId RemoteReplicationStore::AllocateNetId()
    {
        if ( m_LeaseNext >= m_LeaseEnd )
        {
            // A sim reports Ready only once it holds a lease, so no player joins before one; a block of 65536 is
            // renewed at half use below, so reaching its end means the store stopped answering.
            m_Log( "store replica: AllocateNetId with no NetId lease left - the store has not answered the "
                   "request" );
            RequestLease();
            return kInvalidNetId;
        }
        const NetId id = m_LeaseNext++;
        if ( m_LeaseEnd - m_LeaseNext == kLeaseSize / 2 )
        {
            m_LeaseAsked = false;
            RequestLease();
        }
        return id;
    }

    void RemoteReplicationStore::Write( const PawnState& state )
    {
        m_Pawns[state.Id] = state;
        m_Endpoint.SendToRole( EMeshRole::Store, EncodePawn( EMesh::PawnWrite, state ) );
    }

    void RemoteReplicationStore::Remove( NetId id )
    {
        m_Pawns.erase( id );
        m_Endpoint.SendToRole( EMeshRole::Store, EncodeU32( EMesh::PawnRemove, id ) );
    }

    std::vector<PawnState> RemoteReplicationStore::All() const
    {
        std::vector<PawnState> out;
        out.reserve( m_Pawns.size() );
        for ( const auto& [id, pawn] : m_Pawns )
            out.push_back( pawn );
        return out;
    }

    std::uint32_t RemoteReplicationStore::WriteStable( StableNetId id, std::string_view component,
                                                       std::string_view field, const ReplicatedValue& value )
    {
        if ( const auto object = m_Stable.find( id ); object != m_Stable.end() )
            for ( const FieldUpdate& f : object->second )
                if ( f.Component == component && f.Field == field && f.Value == value )
                    return f.Version;
        Writer w( EMesh::StableWrite );
        w.U64( id );
        w.Str( component );
        w.Str( field );
        w.Value( value );
        m_Endpoint.SendToRole( EMeshRole::Store, w.Take() );
        return 0;
    }

    bool RemoteReplicationStore::DestroyStable( StableNetId id )
    {
        if ( m_Destroyed.contains( id ) )
            return false;
        Writer w( EMesh::StableDestroy );
        w.U64( id );
        m_Endpoint.SendToRole( EMeshRole::Store, w.Take() );
        m_Destroyed.insert( id );
        m_Stable.erase( id );
        return true;
    }

    bool RemoteReplicationStore::OnStoreMessage( std::uint8_t type, std::span<const std::byte> body )
    {
        Reader r( body );
        if ( type == Code( EMesh::Lease ) )
        {
            std::uint32_t first = 0, count = 0;
            if ( r.U32( first ) && r.U32( count ) )
            {
                m_LeaseNext = first;
                m_LeaseEnd  = first + count;
                m_Log( std::format( "store replica: NetId block [{}, {}) leased by the store", first,
                                    first + count ) );
            }
            return true;
        }
        if ( type == Code( EMesh::PawnWrite ) )
        {
            PawnState pawn;
            if ( r.Pawn( pawn ) )
                m_Pawns[pawn.Id] = pawn;
            return true;
        }
        if ( type == Code( EMesh::PawnRemove ) )
        {
            std::uint32_t id = 0;
            if ( r.U32( id ) )
                m_Pawns.erase( id );
            return true;
        }
        if ( type == Code( EMesh::StableChanged ) )
        {
            FieldUpdate update;
            StableNetId id = 0;
            if ( !r.U64( id ) || !r.Str( update.Component ) || !r.Str( update.Field ) ||
                 !r.U32( update.Version ) || !r.Value( update.Value ) )
                return true;
            std::vector<FieldUpdate>& fields = m_Stable[id];
            const auto                it = std::find_if( fields.begin(), fields.end(), [&]( const FieldUpdate& f )
                                                         { return f.Component == update.Component && f.Field == update.Field; } );
            if ( it == fields.end() )
                fields.push_back( std::move( update ) );
            else
                *it = std::move( update );
            return true;
        }
        if ( type == Code( EMesh::StableDestroy ) )
        {
            StableNetId id = 0;
            if ( r.U64( id ) )
            {
                m_Destroyed.insert( id );
                m_Stable.erase( id );
            }
            return true;
        }
        if ( type == Code( EMesh::Synced ) )
        {
            m_Synced = true;
            m_Log( std::format(
                 "store replica: in sync with the store - {} pawn(s), {} stable object(s), {} destroyed",
                 m_Pawns.size(), m_Stable.size(), m_Destroyed.size() ) );
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------------------------- RemoteSimLink

    RemoteSimLink::RemoteSimLink( MeshEndpoint& endpoint, NetLogSink log )
         : m_Endpoint( endpoint ), m_Log( std::move( log ) )
    {
    }

    std::optional<ConnectionId> RemoteSimLink::Route( PlayerId player, std::string_view what )
    {
        // THE AUTHORITY MAP decides: the region the player's pawn is in (one region, the world, in NET-MESH-1).
        if ( m_Regions.empty() )
        {
            m_Log( std::format( "gateway: {} of player {} dropped - the authority map names no sim", what,
                                player ) );
            return std::nullopt;
        }
        const MeshRegion& region     = m_Regions.front();
        const auto        connection = m_Endpoint.PeerConnection( EMeshRole::Sim, region.Sim );
        const auto        routed     = std::make_pair( region.Sim, region.Epoch );
        const auto        last       = m_Routed.find( player );
        if ( last == m_Routed.end() || last->second != routed )
        {
            m_Log( std::format( "gateway: {} player {} -> sim {} (authority map: region '{}', epoch {}){}", what,
                                player, region.Sim, region.Name, region.Epoch,
                                connection ? std::string() : " - no link to that sim yet, dropped" ) );
            if ( connection )
                m_Routed[player] = routed;
        }
        return connection;
    }

    void RemoteSimLink::PlayerJoined( PlayerId player, std::uint64_t account )
    {
        if ( const auto connection = Route( player, "join" ) )
        {
            Writer w( EMesh::PlayerJoined );
            w.U32( player );
            w.U64( account );
            m_Endpoint.Send( *connection, w.Take() );
            m_Routed.erase( player ); // the first input logs its route too
        }
    }

    void RemoteSimLink::PlayerLeft( PlayerId player )
    {
        if ( const auto connection = Route( player, "leave" ) )
            m_Endpoint.Send( *connection, EncodeU32( EMesh::PlayerLeft, player ) );
        m_Routed.erase( player );
        m_Acks.erase( player );
    }

    void RemoteSimLink::PlayerInput( PlayerId player, const InputMessage& input )
    {
        if ( const auto connection = Route( player, "input" ) )
        {
            Writer w( EMesh::PlayerInput );
            w.U32( player );
            w.Bytes( EncodeInput( input ) );
            m_Endpoint.Send( *connection, w.Take() );
        }
    }

    std::optional<ISimLink::AckedMove> RemoteSimLink::LastProcessedInput( PlayerId player ) const
    {
        const auto it = m_Acks.find( player );
        return it == m_Acks.end() ? std::nullopt : std::optional<AckedMove>( it->second );
    }

    bool RemoteSimLink::OnMeshMessage( std::uint8_t type, std::span<const std::byte> body )
    {
        Reader r( body );
        if ( type == Code( EMesh::AuthorityMap ) )
        {
            std::uint32_t           n = 0;
            std::vector<MeshRegion> regions;
            if ( !r.U32( n ) )
                return true;
            for ( std::uint32_t i = 0; i < n; ++i )
            {
                MeshRegion region;
                if ( !r.Str( region.Name ) || !r.U32( region.Sim ) || !r.U32( region.Epoch ) )
                    return true;
                regions.push_back( std::move( region ) );
            }
            m_Regions = std::move( regions );
            for ( const MeshRegion& region : m_Regions )
                m_Log( std::format( "gateway: authority map: region '{}' -> sim {} (epoch {})", region.Name,
                                    region.Sim, region.Epoch ) );
            return true;
        }
        if ( type == Code( EMesh::InputAck ) )
        {
            std::uint32_t player = 0;
            AckedMove     move;
            if ( r.U32( player ) && r.Move( move ) )
                m_Acks[player] = move;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------ RemoteGatewayLink

    RemoteGatewayLink::RemoteGatewayLink( MeshEndpoint& endpoint, NetLogSink log )
         : m_Endpoint( endpoint ), m_Log( std::move( log ) )
    {
    }

    void RemoteGatewayLink::PawnSpawned( PlayerId owner, std::uint32_t player, const PawnState& pawn )
    {
        const auto gateway = m_GatewayOf.find( owner );
        if ( gateway == m_GatewayOf.end() )
            return;
        Writer w( EMesh::PawnSpawned );
        w.U32( owner );
        w.U32( player );
        w.Pawn( pawn );
        if ( const auto connection = m_Endpoint.PeerConnection( EMeshRole::Gateway, gateway->second ) )
            m_Endpoint.Send( *connection, w.Take() );
    }

    void RemoteGatewayLink::PawnDestroyed( NetId id )
    {
        m_Endpoint.SendToRole( EMeshRole::Gateway, EncodeU32( EMesh::PawnDestroyed, id ) );
    }

    bool RemoteGatewayLink::OnGatewayMessage( std::uint32_t gateway, std::uint8_t type,
                                              std::span<const std::byte> body, PawnSimulation& sim )
    {
        Reader        r( body );
        std::uint32_t player = 0;
        if ( type == Code( EMesh::PlayerJoined ) )
        {
            std::uint64_t account = 0;
            if ( r.U32( player ) && r.U64( account ) )
            {
                m_GatewayOf[player] = gateway;
                m_Log( std::format( "sim: player {} (account {}) joined through gateway {}", player, account,
                                    gateway ) );
                sim.PlayerJoined( player, account );
            }
            return true;
        }
        if ( type == Code( EMesh::PlayerLeft ) )
        {
            if ( r.U32( player ) )
            {
                sim.PlayerLeft( player );
                m_GatewayOf.erase( player );
            }
            return true;
        }
        if ( type == Code( EMesh::PlayerInput ) )
        {
            if ( r.U32( player ) && m_GatewayOf.contains( player ) )
                if ( const auto input = DecodeInput( r.Rest() ) )
                    sim.PlayerInput( player, *input );
            return true;
        }
        return false;
    }

    void RemoteGatewayLink::SendAcks( const PawnSimulation& sim )
    {
        for ( const auto& [player, gateway] : m_GatewayOf )
        {
            const auto ack = sim.LastProcessedInput( player );
            if ( !ack )
                continue;
            Writer w( EMesh::InputAck );
            w.U32( player );
            w.Move( *ack );
            if ( const auto connection = m_Endpoint.PeerConnection( EMeshRole::Gateway, gateway ) )
                m_Endpoint.Send( *connection, w.Take() );
        }
    }

    bool DispatchMeshPawnEvent( std::uint8_t type, std::span<const std::byte> body, IGatewayLink& gateway )
    {
        Reader r( body );
        if ( type == Code( EMesh::PawnSpawned ) )
        {
            std::uint32_t owner = 0, player = 0;
            PawnState     pawn;
            if ( r.U32( owner ) && r.U32( player ) && r.Pawn( pawn ) )
                gateway.PawnSpawned( owner, player, pawn );
            return true;
        }
        if ( type == Code( EMesh::PawnDestroyed ) )
        {
            std::uint32_t id = 0;
            if ( r.U32( id ) )
                gateway.PawnDestroyed( id );
            return true;
        }
        return false;
    }
} // namespace Desert::Net

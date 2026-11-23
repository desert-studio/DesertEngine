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
            Lease,            // u32 first, u32 count
            PawnWrite,        // pawn
            PawnRemove,       // u32 id
            StableWrite,      // u64 id, str component, str field, value             (sim -> store)
            StableChanged,    // u64 id, str component, str field, u32 version, value (store -> subscriber)
            StableDestroy,    // u64 id
            Synced,           // the store sent a new subscriber its full state
            PlayerJoined,     // u32 player, u64 account
            PlayerLeft,       // u32 player
            PlayerInput,      // u32 player, the client's Input message as received
            InputAck,         // u32 player, u32 step
            PawnSpawned,      // u32 owner, u32 player, pawn
            PawnDestroyed,    // u32 id
            Handover,         // handover header (epoch = the sender's), handover pawn  (sim -> store)
            Adopt,            // handover header (epoch = the new one), handover pawn   (store -> new owner)
            AuthorityChanged, // u32 id, u32 sim, u32 epoch, u32 step, u32 gateway, u32 player (store -> gateways)
            ForwardInput,     // u32 to sim, u32 player, Input            (old owner -> store)
            ForwardedInput,   // u32 player, Input                        (store -> new owner)
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

        void WriteMap( Writer& w, const MeshAuthorityMap& map )
        {
            w.U32( static_cast<std::uint32_t>( map.Regions.size() ) );
            for ( const MeshRegion& region : map.Regions )
            {
                w.Str( region.Name );
                w.U32( region.Sim );
                w.U32( region.Epoch );
                w.F32( region.MinX );
                w.F32( region.MinZ );
                w.F32( region.MaxX );
                w.F32( region.MaxZ );
            }
            w.F32( map.BorderBand );
            w.F32( map.HandoverMargin );
        }

        bool ReadMap( Reader& r, MeshAuthorityMap& map )
        {
            std::uint32_t n = 0;
            if ( !r.U32( n ) )
                return false;
            for ( std::uint32_t i = 0; i < n; ++i )
            {
                MeshRegion region;
                if ( !r.Str( region.Name ) || !r.U32( region.Sim ) || !r.U32( region.Epoch ) ||
                     !r.F32( region.MinX ) || !r.F32( region.MinZ ) || !r.F32( region.MaxX ) ||
                     !r.F32( region.MaxZ ) )
                    return false;
                map.Regions.push_back( std::move( region ) );
            }
            return r.F32( map.BorderBand ) && r.F32( map.HandoverMargin );
        }

        std::string DescribeRegion( const MeshRegion& region )
        {
            if ( region.MinX == -std::numeric_limits<float>::max() &&
                 region.MaxX == std::numeric_limits<float>::max() &&
                 region.MinZ == -std::numeric_limits<float>::max() &&
                 region.MaxZ == std::numeric_limits<float>::max() )
                return std::format( "region '{}' -> sim {} (epoch {})", region.Name, region.Sim, region.Epoch );
            return std::format( "region '{}' [x {:.0f}..{:.0f}, z {:.0f}..{:.0f} cm] -> sim {} (epoch {})",
                                region.Name, region.MinX, region.MaxX, region.MinZ, region.MaxZ, region.Sim,
                                region.Epoch );
        }

        // A handover in flight: who hands which pawn to whom, at which mesh step; the gateway the player is on.
        struct HandoverHeader
        {
            std::uint32_t From = 0, Epoch = 0, To = 0, Step = 0, Gateway = 0;
        };

        void WriteInputStep( Writer& w, const InputStep& input )
        {
            w.U32( input.Step );
            w.F32( input.Move.X );
            w.F32( input.Move.Y );
            w.F32( input.Move.Z );
            w.F32( input.Yaw );
        }

        bool ReadInputStep( Reader& r, InputStep& input )
        {
            return r.U32( input.Step ) && r.F32( input.Move.X ) && r.F32( input.Move.Y ) &&
                   r.F32( input.Move.Z ) && r.F32( input.Yaw );
        }

        std::vector<std::byte> EncodeHandover( EMesh type, const HandoverHeader& h,
                                               const PawnSimulation::HandoverPawn& pawn )
        {
            Writer w( type );
            w.U32( h.From );
            w.U32( h.Epoch );
            w.U32( h.To );
            w.U32( h.Step );
            w.U32( h.Gateway );
            w.U32( pawn.Player );
            w.U64( pawn.Account );
            w.U32( pawn.Index );
            w.Pawn( pawn.State );
            w.U8( pawn.LastProcessed ? 1 : 0 );
            w.U32( pawn.LastProcessed.value_or( 0 ) );
            WriteInputStep( w, pawn.LastApplied );
            w.U32( static_cast<std::uint32_t>( pawn.Pending.size() ) );
            for ( const InputStep& input : pawn.Pending )
                WriteInputStep( w, input );
            return w.Take();
        }

        bool DecodeHandover( std::span<const std::byte> body, HandoverHeader& h,
                             PawnSimulation::HandoverPawn& pawn )
        {
            Reader        r( body );
            std::uint8_t  hasProcessed = 0;
            std::uint32_t processed = 0, pending = 0;
            if ( !r.U32( h.From ) || !r.U32( h.Epoch ) || !r.U32( h.To ) || !r.U32( h.Step ) ||
                 !r.U32( h.Gateway ) || !r.U32( pawn.Player ) || !r.U64( pawn.Account ) || !r.U32( pawn.Index ) ||
                 !r.Pawn( pawn.State ) || !r.U8( hasProcessed ) || !r.U32( processed ) ||
                 !ReadInputStep( r, pawn.LastApplied ) || !r.U32( pending ) || pending > InputRing::kCapacity )
                return false;
            if ( hasProcessed )
                pawn.LastProcessed = processed;
            pawn.Pending.resize( pending );
            for ( InputStep& input : pawn.Pending )
                if ( !ReadInputStep( r, input ) )
                    return false;
            return true;
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

    const MeshRegion* MeshAuthorityMap::RegionAt( float x, float z ) const
    {
        for ( const MeshRegion& region : Regions )
            if ( region.Contains( x, z ) )
                return &region;
        return nullptr;
    }

    bool MeshAuthorityMap::InBandOf( std::uint32_t sim, float x, float z ) const
    {
        for ( const MeshRegion& r : Regions )
            if ( r.Sim == sim && x >= r.MinX - BorderBand && x < r.MaxX + BorderBand && z >= r.MinZ - BorderBand &&
                 z < r.MaxZ + BorderBand )
                return true;
        return false;
    }

    std::optional<std::uint32_t> MeshAuthorityMap::HandoverTarget( std::uint32_t owner, float x, float z ) const
    {
        const MeshRegion* region = RegionAt( x, z );
        if ( !region || region->Sim == owner || region->Depth( x, z ) <= HandoverMargin )
            return std::nullopt;
        return region->Sim;
    }

    std::optional<MeshAuthorityMap> DecodeMeshAuthorityMap( std::uint8_t type, std::span<const std::byte> body )
    {
        if ( type != Code( EMesh::AuthorityMap ) )
            return std::nullopt;
        Reader           r( body );
        MeshAuthorityMap map;
        if ( !ReadMap( r, map ) )
            return std::nullopt;
        return map;
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

    MeshCoordinator::MeshCoordinator( MeshEndpoint& endpoint, MeshRegionTable table, NetLogSink log )
         : m_Endpoint( endpoint ), m_Log( std::move( log ) ), m_Table( std::move( table ) )
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
        // THE AUTHORITY MAP: the static region table (§2M.3), each region in the map once its sim is ready; with
        // no table, one region - the whole world - owned by the lowest ready sim, its epoch moving on every change
        // of owner.
        MeshAuthorityMap map;
        map.BorderBand     = m_Table.BorderBand;
        map.HandoverMargin = m_Table.HandoverMargin;
        if ( m_Table.Regions.empty() )
        {
            if ( !m_ReadySims.empty() )
            {
                const std::uint32_t sim   = *m_ReadySims.begin();
                std::uint32_t       epoch = m_Map.Regions.empty() ? 1 : m_Map.Regions.front().Epoch;
                if ( !m_Map.Regions.empty() && m_Map.Regions.front().Sim != sim )
                    ++epoch;
                MeshRegion world;
                world.Name  = "world";
                world.Sim   = sim;
                world.Epoch = epoch;
                map.Regions.push_back( std::move( world ) );
            }
        }
        else
            for ( const MeshRegion& region : m_Table.Regions )
                if ( m_ReadySims.contains( region.Sim ) )
                    map.Regions.push_back( region );
        bool mapChanged = map.Regions.size() != m_Map.Regions.size();
        for ( std::size_t i = 0; !mapChanged && i < map.Regions.size(); ++i )
            mapChanged = map.Regions[i].Name != m_Map.Regions[i].Name ||
                         map.Regions[i].Sim != m_Map.Regions[i].Sim ||
                         map.Regions[i].Epoch != m_Map.Regions[i].Epoch;
        m_Map = std::move( map );
        if ( mapChanged )
        {
            if ( m_Map.Regions.empty() )
                m_Log( "coordinator: authority map empty (no ready sim)" );
            for ( const MeshRegion& region : m_Map.Regions )
                m_Log( std::format( "coordinator: authority map: {}{}", DescribeRegion( region ),
                                    m_Table.Regions.empty()
                                         ? std::string()
                                         : std::format( "; border band {:.0f} cm, handover margin {:.0f} cm",
                                                        m_Map.BorderBand, m_Map.HandoverMargin ) ) );
        }

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
        Writer                       mapWriter( EMesh::AuthorityMap );
        WriteMap( mapWriter, m_Map );
        const std::vector<std::byte> mapMessage = mapWriter.Take();
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
                            m_ReadySims.size(), m_Map.Regions.size() );
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
        if ( const auto map = DecodeMeshAuthorityMap( type, body ) )
        {
            m_Map = *map;
            for ( const MeshRegion& region : m_Map.Regions )
                m_Log( std::format( "store: authority map from the coordinator: {}", DescribeRegion( region ) ) );
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
            const auto [record, created] = m_PawnAuthority.emplace( pawn.Id, Authority{ index, 1 } );
            if ( created )
                m_Log( std::format( "store: pawn net id {} - authority (sim {}, epoch 1)", pawn.Id, index ) );
            else if ( record->second.Sim != index )
            {
                // A write of an epoch that is over (the sim has not seen its handover land yet): refused.
                if ( m_Refused.insert( { pawn.Id, index } ).second )
                    m_Log( std::format( "store: pawn net id {} - write from sim {} refused, authority is (sim {}, "
                                        "epoch {})",
                                        pawn.Id, index, record->second.Sim, record->second.Epoch ) );
                return;
            }
            m_State.Write( pawn );
            m_Endpoint.SendToRole( EMeshRole::Gateway, EncodePawn( EMesh::PawnWrite, pawn ) );
            StreamProxies( pawn, index );
        }
        else if ( type == Code( EMesh::PawnRemove ) )
        {
            std::uint32_t id = 0;
            if ( !r.U32( id ) )
                return;
            if ( const auto record = m_PawnAuthority.find( id );
                 record != m_PawnAuthority.end() && record->second.Sim != index )
                return;
            m_State.Remove( id );
            m_PawnAuthority.erase( id );
            m_Endpoint.SendToRole( EMeshRole::Gateway, EncodeU32( EMesh::PawnRemove, id ) );
            for ( auto& [sim, proxies] : m_Proxies )
                if ( proxies.erase( id ) )
                    if ( const auto connection = m_Endpoint.PeerConnection( EMeshRole::Sim, sim ) )
                        m_Endpoint.Send( *connection, EncodeU32( EMesh::PawnRemove, id ) );
        }
        else if ( type == Code( EMesh::Handover ) )
            Handover( index, body );
        else if ( type == Code( EMesh::ForwardInput ) )
        {
            std::uint32_t to = 0, player = 0;
            if ( !r.U32( to ) || !r.U32( player ) )
                return;
            Writer w( EMesh::ForwardedInput );
            w.U32( player );
            w.Bytes( r.Rest() );
            if ( const auto connection = m_Endpoint.PeerConnection( EMeshRole::Sim, to ) )
                m_Endpoint.Send( *connection, w.Take() );
            m_Log( std::format( "store: input of player {} that reached sim {} after its handover -> forwarded to "
                                "sim {}",
                                player, index, to ) );
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

    void MeshStore::StreamProxies( const PawnState& pawn, std::uint32_t owner )
    {
        // THE BORDER BAND (§2M.5): a sim holds, read-only, the pawns of others standing within the band of its
        // regions - its interest by cell, here by the region rectangles the cells are cut into.
        for ( const std::uint32_t sim : m_Endpoint.PeersOfRole( EMeshRole::Sim ) )
        {
            if ( sim == owner )
                continue;
            std::set<NetId>& proxies    = m_Proxies[sim];
            const auto       connection = m_Endpoint.PeerConnection( EMeshRole::Sim, sim );
            if ( !connection )
                continue;
            if ( m_Map.InBandOf( sim, pawn.Position.X, pawn.Position.Z ) )
            {
                proxies.insert( pawn.Id );
                m_Endpoint.Send( *connection, EncodePawn( EMesh::PawnWrite, pawn ) );
            }
            else if ( proxies.erase( pawn.Id ) )
                m_Endpoint.Send( *connection, EncodeU32( EMesh::PawnRemove, pawn.Id ) );
        }
    }

    void MeshStore::Handover( std::uint32_t from, std::span<const std::byte> body )
    {
        HandoverHeader               h;
        PawnSimulation::HandoverPawn pawn;
        if ( !DecodeHandover( body, h, pawn ) )
            return;
        const NetId id     = pawn.State.Id;
        const auto  record = m_PawnAuthority.find( id );
        const auto  target = m_Endpoint.PeerConnection( EMeshRole::Sim, h.To );
        // THE COMPARE-AND-SWAP of the authority record: only the holder of the current epoch hands over, to a
        // sim the store can reach; anything else is refused, and a refused holder gets its pawn back.
        if ( record == m_PawnAuthority.end() || record->second.Sim != from || record->second.Epoch != h.Epoch ||
             !target )
        {
            m_Log(
                 std::format( "store: handover of pawn net id {} from (sim {}, epoch {}) to sim {} at step {} "
                              "refused - the record is {}",
                              id, from, h.Epoch, h.To, h.Step,
                              record == m_PawnAuthority.end()
                                   ? std::string( "missing" )
                                   : std::format( "(sim {}, epoch {}){}", record->second.Sim, record->second.Epoch,
                                                  target ? "" : ", no link to the target sim" ) ) );
            if ( record != m_PawnAuthority.end() && record->second.Sim == from )
            {
                h.To    = from;
                h.Epoch = record->second.Epoch;
                if ( const auto back = m_Endpoint.PeerConnection( EMeshRole::Sim, from ) )
                    m_Endpoint.Send( *back, EncodeHandover( EMesh::Adopt, h, pawn ) );
            }
            return;
        }
        record->second = { h.To, h.Epoch + 1 };
        m_Refused.erase( { id, h.To } );
        m_Proxies[h.To].erase( id );
        m_State.Write( pawn.State );
        m_Log( std::format(
             "store: pawn net id {} (player {} of gateway {}) authority (sim {}, epoch {}) -> (sim {}, "
             "epoch {}) at step {} - full state at ({:.0f}, {:.0f}, {:.0f}) cm + {} unacked input(s) "
             "handed to sim {}",
             id, pawn.Player, h.Gateway, from, h.Epoch, h.To, h.Epoch + 1, h.Step, pawn.State.Position.X,
             pawn.State.Position.Y, pawn.State.Position.Z, pawn.Pending.size(), h.To ) );
        HandoverHeader adopt = h;
        adopt.Epoch          = h.Epoch + 1;
        m_Endpoint.Send( *target, EncodeHandover( EMesh::Adopt, adopt, pawn ) );
        Writer changed( EMesh::AuthorityChanged );
        changed.U32( id );
        changed.U32( h.To );
        changed.U32( adopt.Epoch );
        changed.U32( h.Step );
        changed.U32( h.Gateway );
        changed.U32( pawn.Player );
        m_Endpoint.SendToRole( EMeshRole::Gateway, changed.Take() );
        m_Endpoint.SendToRole( EMeshRole::Gateway, EncodePawn( EMesh::PawnWrite, pawn.State ) );
        StreamProxies( pawn.State, h.To );
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
            if ( !r.Pawn( pawn ) )
                return true;
            m_Pawns[pawn.Id] = pawn;
            if ( m_TrackProxies && m_Proxies.insert( pawn.Id ).second )
                m_Log( std::format( "store replica: proxy pawn net id {} at ({:.0f}, {:.0f}, {:.0f}) cm - in this "
                                    "sim's border band, authority elsewhere (read-only)",
                                    pawn.Id, pawn.Position.X, pawn.Position.Y, pawn.Position.Z ) );
            return true;
        }
        if ( type == Code( EMesh::PawnRemove ) )
        {
            std::uint32_t id = 0;
            if ( !r.U32( id ) )
                return true;
            m_Pawns.erase( id );
            if ( m_TrackProxies && m_Proxies.erase( id ) )
                m_Log( std::format( "store replica: proxy pawn net id {} left this sim's border band", id ) );
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
        // THE AUTHORITY RECORD of the player's pawn decides (the store's AuthorityChanged, §2M.4); a player whose
        // pawn never changed owner is on the entry region's sim, where it joined.
        if ( m_Map.Regions.empty() )
        {
            m_Log( std::format( "gateway: {} of player {} dropped - the authority map names no sim", what,
                                player ) );
            return std::nullopt;
        }
        const MeshRegion&                             entry     = m_Map.Regions.front();
        const auto                                    authority = m_Authority.find( player );
        const std::pair<std::uint32_t, std::uint32_t> routed =
             authority != m_Authority.end() ? authority->second : std::make_pair( entry.Sim, entry.Epoch );
        const auto connection = m_Endpoint.PeerConnection( EMeshRole::Sim, routed.first );
        const auto last       = m_Routed.find( player );
        if ( last == m_Routed.end() || last->second != routed )
        {
            m_Log( std::format(
                 "gateway: {} player {} -> sim {} ({}){}", what, player, routed.first,
                 authority != m_Authority.end()
                      ? std::format( "authority of its pawn: epoch {}", routed.second )
                      : std::format( "authority map: region '{}', epoch {}", entry.Name, entry.Epoch ),
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
        m_Authority.erase( player );
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

    std::optional<std::uint32_t> RemoteSimLink::LastProcessedInput( PlayerId player ) const
    {
        const auto it = m_Acks.find( player );
        return it == m_Acks.end() ? std::nullopt : std::optional<std::uint32_t>( it->second );
    }

    bool RemoteSimLink::OnMeshMessage( std::uint8_t type, std::span<const std::byte> body )
    {
        Reader r( body );
        if ( auto map = DecodeMeshAuthorityMap( type, body ) )
        {
            m_Map = std::move( *map );
            for ( const MeshRegion& region : m_Map.Regions )
                m_Log( std::format( "gateway: authority map: {}", DescribeRegion( region ) ) );
            return true;
        }
        if ( type == Code( EMesh::InputAck ) )
        {
            std::uint32_t player = 0, step = 0;
            if ( r.U32( player ) && r.U32( step ) )
                m_Acks[player] = step;
            return true;
        }
        return false;
    }

    bool RemoteSimLink::OnStoreMessage( std::uint32_t gateway, std::uint8_t type, std::span<const std::byte> body )
    {
        if ( type != Code( EMesh::AuthorityChanged ) )
            return false;
        Reader        r( body );
        std::uint32_t id = 0, sim = 0, epoch = 0, step = 0, on = 0, player = 0;
        if ( !r.U32( id ) || !r.U32( sim ) || !r.U32( epoch ) || !r.U32( step ) || !r.U32( on ) ||
             !r.U32( player ) || on != gateway )
            return true;
        m_Authority[player] = { sim, epoch };
        m_Log( std::format(
             "gateway: player {} pawn net id {} - authority (sim {}, epoch {}) from step {}: its inputs "
             "re-routed to sim {}",
             player, id, sim, epoch, step, sim ) );
        return true;
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
                {
                    // Sent before the gateway saw the handover: on to the pawn's owner, by its steps (§2M.4).
                    const auto forward = m_ForwardTo.find( player );
                    if ( !sim.HasPawn( player ) && forward != m_ForwardTo.end() )
                    {
                        Writer w( EMesh::ForwardInput );
                        w.U32( forward->second );
                        w.U32( player );
                        w.Bytes( EncodeInput( *input ) );
                        m_Endpoint.SendToRole( EMeshRole::Store, w.Take() );
                    }
                    else
                        sim.PlayerInput( player, *input );
                }
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
            w.U32( *ack );
            if ( const auto connection = m_Endpoint.PeerConnection( EMeshRole::Gateway, gateway ) )
                m_Endpoint.Send( *connection, w.Take() );
        }
    }

    std::optional<std::uint32_t> RemoteGatewayLink::GatewayOf( PlayerId player ) const
    {
        const auto it = m_GatewayOf.find( player );
        return it == m_GatewayOf.end() ? std::nullopt : std::optional<std::uint32_t>( it->second );
    }

    void RemoteGatewayLink::ForwardInputs( PlayerId player, std::uint32_t sim )
    {
        m_ForwardTo[player] = sim;
    }

    void RemoteGatewayLink::Adopted( PlayerId player, std::uint32_t gateway )
    {
        m_GatewayOf[player] = gateway;
        m_ForwardTo.erase( player );
    }

    // ------------------------------------------------------------------------------------------------ SimHandover

    SimHandover::SimHandover( MeshEndpoint& endpoint, std::uint32_t sim, NetLogSink log )
         : m_Endpoint( endpoint ), m_Sim( sim ), m_Log( std::move( log ) )
    {
    }

    bool SimHandover::OnCoordinatorMessage( std::uint8_t type, std::span<const std::byte> body )
    {
        auto map = DecodeMeshAuthorityMap( type, body );
        if ( !map )
            return false;
        m_Map = std::move( *map );
        for ( const MeshRegion& region : m_Map.Regions )
            m_Log( std::format( "sim {}: authority map: {}{}", m_Sim, DescribeRegion( region ),
                                region.Sim == m_Sim ? " (this sim)" : "" ) );
        return true;
    }

    void SimHandover::AfterStep( std::uint32_t step, PawnSimulation& sim, RemoteGatewayLink& gateways,
                                 RemoteReplicationStore& replica )
    {
        for ( const PawnSimulation::Source& source : sim.Sources() )
        {
            const auto target  = m_Map.HandoverTarget( m_Sim, source.Position.X, source.Position.Z );
            const auto gateway = gateways.GatewayOf( source.Player );
            if ( !target || !gateway )
                continue;
            const MeshRegion* region = m_Map.RegionAt( source.Position.X, source.Position.Z );
            auto              pawn   = sim.Release( source.Player );
            if ( !pawn || !region )
                continue;
            const auto          epochIt = m_Epoch.find( source.Player );
            const std::uint32_t epoch   = epochIt == m_Epoch.end() ? 1 : epochIt->second;
            m_Log( std::format(
                 "sim {}: pawn net id {} (player {}) at ({:.0f}, {:.0f}) cm is {:.0f} cm inside region "
                 "'{}' of sim {} (margin {:.0f}) - frozen at step {} and handed over through the store "
                 "(epoch {}, {} unacked input(s))",
                 m_Sim, source.Id, source.Player, source.Position.X, source.Position.Z,
                 region->Depth( source.Position.X, source.Position.Z ), region->Name, *target,
                 m_Map.HandoverMargin, step, epoch, pawn->Pending.size() ) );
            m_Endpoint.SendToRole(
                 EMeshRole::Store,
                 EncodeHandover( EMesh::Handover, { m_Sim, epoch, *target, step, *gateway }, *pawn ) );
            gateways.ForwardInputs( source.Player, *target );
            replica.Forget( source.Id );
            m_Epoch.erase( source.Player );
        }
    }

    bool SimHandover::OnStoreMessage( std::uint8_t type, std::span<const std::byte> body, PawnSimulation& sim,
                                      RemoteGatewayLink& gateways, RemoteReplicationStore& replica,
                                      std::uint32_t step, double stepSeconds )
    {
        if ( type == Code( EMesh::Adopt ) )
        {
            HandoverHeader               h;
            PawnSimulation::HandoverPawn pawn;
            if ( !DecodeHandover( body, h, pawn ) )
                return true;
            const bool          wasProxy = replica.IsProxy( pawn.State.Id );
            const std::uint32_t catchUp  = step > h.Step ? step - h.Step : 0;
            replica.Promote( pawn.State.Id );
            m_Epoch[pawn.Player] = h.Epoch;
            gateways.Adopted( pawn.Player, h.Gateway );
            m_Log( std::format(
                 "sim {}: adopted pawn net id {} (player {}) from sim {} at epoch {} - {}; frozen at step "
                 "{}, this sim is at step {}: {} catch-up step(s), {} unacked input(s)",
                 m_Sim, pawn.State.Id, pawn.Player, h.From, h.Epoch,
                 wasProxy ? "its proxy promoted to authoritative"
                          : "held no proxy of it (outside the border band), placed from the handed state",
                 h.Step, step, catchUp, pawn.Pending.size() ) );
            sim.Adopt( std::move( pawn ), catchUp, stepSeconds );
            return true;
        }
        if ( type == Code( EMesh::ForwardedInput ) )
        {
            Reader        r( body );
            std::uint32_t player = 0;
            if ( r.U32( player ) )
                if ( const auto input = DecodeInput( r.Rest() ) )
                    sim.PlayerInput( player, *input );
            return true;
        }
        return false;
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

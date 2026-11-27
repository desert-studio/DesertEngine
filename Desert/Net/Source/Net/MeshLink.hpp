#pragma once

// THE MESH LINKS (Docs/contracts/NET-ARCH.md §2M.2, NET-MESH-1): the server roles as their own processes -
//   DesertServer --role coordinator   the authority map (region -> sim, epoch) and the mesh step clock
//   DesertServer --role store         the replication store: latest pawn + stable state, NetId block leases,
//                                     the change stream to every subscriber
//   DesertServer --role sim           PawnSimulation + the world's streaming; writes to the store
//   DesertServer --role gateway       the client connections (Gateway), routing a player's input to the sim the
//                                     authority map names
// linked over GNS connections that carry a CA-signed certificate (MeshCertificates.hpp): a mesh peer whose
// certificate the dev CA did not sign, or whose identity is not the role it says it is, is closed. The interfaces
// of the mesh of one (IReplicationStore, ISimLink, IGatewayLink) get remote implementations here, so Gateway and
// PawnSimulation run unchanged in either layout. SpatialOS runtime / Star Citizen server meshing: clients talk to
// a gateway only, sims own entities by an authority map, a store holds the state between them.
//
// Who connects to whom: every role connects to the coordinator (--coordinator ip:port) and says Hello with its
// role, index and mesh port; the coordinator answers with the Directory (where the store and the ready sims
// listen). A sim connects to the store, receives its NetId lease and then reports Ready; the coordinator puts
// ready sims in the authority map. A gateway connects to the store (the full state, then the change stream) and to
// every sim in the map, and listens for clients only once the map names a sim it is connected to.

#include <Net/HandoverWire.hpp>
#include <Net/MeshOfOne.hpp>
#include <Net/NetTransport.hpp>
#include <Net/Replication.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    namespace Handover
    {
        class Ledger; // HandoverLedger.hpp (which includes this header for EMeshRole)
        struct Effects;
    } // namespace Handover

    enum class EMeshRole : std::uint8_t
    {
        Coordinator,
        Store,
        Sim,
        Gateway,
    };
    [[nodiscard]] std::string_view         MeshRoleName( EMeshRole role );
    [[nodiscard]] std::optional<EMeshRole> MeshRoleFromName( std::string_view name );
    // The certificate identity of a role process ("mesh-sim-1"); the GNS identity is "str:" + this.
    [[nodiscard]] std::string MeshIdentity( EMeshRole role, std::uint32_t index );

    // One region of the authority map (§2M.3): an aligned block of the WP runtime grid on the ground plane (X, Z
    // in world cm), owned by one sim. Without a region table the map is one region, the whole world (NET-MESH-1).
    struct MeshRegion
    {
        std::string   Name;
        std::uint32_t Sim   = 0; // the sim index with authority
        std::uint32_t Epoch = 0; // bumped on every reassignment of the region (dynamic meshing, NET-MESH-6)
        float         MinX = -std::numeric_limits<float>::max(), MinZ = -std::numeric_limits<float>::max();
        float         MaxX = std::numeric_limits<float>::max(), MaxZ = std::numeric_limits<float>::max();

        [[nodiscard]] bool Contains( float x, float z ) const
        {
            return x >= MinX && x < MaxX && z >= MinZ && z < MaxZ;
        }
        // How far (x, z) lies inside the region: the distance to its nearest edge, negative outside.
        [[nodiscard]] float Depth( float x, float z ) const
        {
            return std::min( std::min( x - MinX, MaxX - x ), std::min( z - MinZ, MaxZ - z ) );
        }
    };
    // THE AUTHORITY MAP as the coordinator publishes it: the regions of ready sims, the border band a sim streams
    // around its regions (proxies of the neighbours' entities inside it, §2M.5) and the hysteresis margin of a
    // handover (an entity changes owner only once it is that far inside the neighbour's region, §2M.4: half a
    // cell of the grid the table is cut on - no ping-pong on the line).
    struct MeshAuthorityMap
    {
        std::vector<MeshRegion> Regions;              // the first one is the entry: players join on its sim
        float                   BorderBand     = 0.f; // cm
        float                   HandoverMargin = 0.f; // cm

        [[nodiscard]] const MeshRegion* RegionAt( float x, float z ) const;
        // True when (x, z) is inside the border band of @p sim's regions (its regions grown by BorderBand).
        [[nodiscard]] bool InBandOf( std::uint32_t sim, float x, float z ) const;
        // The sim an entity at (x, z) owned by @p owner hands over to: the owner of the region it stands in, once
        // it is more than HandoverMargin inside it; nullopt while it stays with @p owner.
        [[nodiscard]] std::optional<std::uint32_t> HandoverTarget( std::uint32_t owner, float x, float z ) const;
    };
    // "region 'east' [x 0..100000, z -100000..100000 cm] -> sim 2 (epoch 1)" for a log line.
    [[nodiscard]] std::string DescribeMeshRegion( const MeshRegion& region );
    // The coordinator's static region table (--region, --border-band, --handover-margin of `--role coordinator`).
    struct MeshRegionTable
    {
        std::vector<MeshRegion> Regions; // empty: one region, the whole world, on the lowest ready sim
        float                   BorderBand     = 0.f;
        float                   HandoverMargin = 0.f;
    };
    struct MeshPeerAddress
    {
        EMeshRole     Role  = EMeshRole::Sim;
        std::uint32_t Index = 0;
        std::string   Address; // "ip:port" of its mesh listen socket
    };

    // The mesh connections of one role process, over the process's one transport. In a gateway, connections it did
    // not open are clients and go to @p clients; in every other role, every connection is a mesh peer.
    class MeshEndpoint final : public INetTransportEvents
    {
    public:
        struct Handler
        {
            std::function<void( EMeshRole, std::uint32_t, ConnectionId, std::uint16_t meshPort )> PeerUp;
            std::function<void( EMeshRole, std::uint32_t, ConnectionId )>                         PeerLost;
            std::function<void( EMeshRole, std::uint32_t, ConnectionId, std::uint8_t type,
                                std::span<const std::byte> body )>
                 Message;
        };
        MeshEndpoint( INetTransport& transport, EMeshRole role, std::uint32_t index, std::uint16_t meshPort,
                      NetLogSink log );
        void SetHandler( Handler handler )
        {
            m_Handler = std::move( handler );
        }
        void SetClients( INetTransportEvents* clients )
        {
            m_Clients = clients;
        }

        // Opens a mesh link to the peer; its PeerUp comes once the certificate checked out and the Hello went.
        bool ConnectTo( EMeshRole role, std::uint32_t index, std::string_view address, std::string& error );
        [[nodiscard]] std::optional<ConnectionId> PeerConnection( EMeshRole role, std::uint32_t index ) const;
        void Send( ConnectionId connection, const std::vector<std::byte>& message );
        void SendToRole( EMeshRole role, const std::vector<std::byte>& message );
        [[nodiscard]] std::vector<std::uint32_t> PeersOfRole( EMeshRole role ) const;
        [[nodiscard]] std::string                AddressOf( ConnectionId connection ) const;

        void OnConnected( ConnectionId connection, std::string_view address ) override;
        void OnClosed( ConnectionId connection, const DisconnectInfo& info ) override;
        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                        NetClock::time_point received ) override;

    private:
        struct Link
        {
            bool          Outgoing = false;
            bool          Up       = false; // Hello exchanged
            EMeshRole     Role     = EMeshRole::Sim;
            std::uint32_t Index    = 0;
            std::string   Address;
        };
        bool Authenticate( ConnectionId connection, EMeshRole role, std::uint32_t index );

        INetTransport&               m_Transport;
        EMeshRole                    m_Role;
        std::uint32_t                m_Index;
        std::uint16_t                m_MeshPort;
        NetLogSink                   m_Log;
        Handler                      m_Handler;
        INetTransportEvents*         m_Clients = nullptr;
        std::map<ConnectionId, Link> m_Links;
        std::set<ConnectionId>       m_ClientConnections;
    };

    // THE COORDINATOR (§2M.2): the directory of the mesh, the authority map and the step clock. Control plane
    // only.
    class MeshCoordinator
    {
    public:
        MeshCoordinator( MeshEndpoint& endpoint, MeshRegionTable table, NetLogSink log );
        // Sends step @p step of the mesh clock to every sim and gateway (and the store).
        void                      Tick( std::uint32_t step );
        [[nodiscard]] std::string Describe() const;

    private:
        void PeerUp( EMeshRole role, std::uint32_t index, ConnectionId connection, std::uint16_t meshPort );
        void PeerLost( EMeshRole role, std::uint32_t index );
        void Message( EMeshRole role, std::uint32_t index, std::uint8_t type, std::span<const std::byte> body );
        void Publish();

        MeshEndpoint&                                                  m_Endpoint;
        NetLogSink                                                     m_Log;
        std::map<std::pair<EMeshRole, std::uint32_t>, MeshPeerAddress> m_Peers;
        std::set<std::uint32_t>                                        m_ReadySims;
        MeshRegionTable                                                m_Table;
        MeshAuthorityMap                                               m_Map;
    };

    // THE STORE ROLE (§2M.2): the state of record, written by the sims, streamed to every subscriber (sims and
    // gateways; partitioned by cell from NET-MESH-2 - one node holds every cell here). A subscriber gets the full
    // state when it connects, then every change in write order. Who may write a pawn, and where a pawn's authority
    // moves, is its Handover::Ledger's (HandoverLedger.hpp); the store performs the effects the ledger returns.
    class MeshStore
    {
    public:
        MeshStore( MeshEndpoint& endpoint, NetLogSink log );
        ~MeshStore();
        MeshStore( const MeshStore& )            = delete;
        MeshStore& operator=( const MeshStore& ) = delete;
        [[nodiscard]] const InProcessReplicationStore& State() const
        {
            return m_State;
        }

    private:
        void PeerUp( EMeshRole role, std::uint32_t index, ConnectionId connection );
        void Message( EMeshRole role, std::uint32_t index, std::uint8_t type, std::span<const std::byte> body );
        void Broadcast( const std::vector<std::byte>& message );
        // The pawn's state to the sims whose border band it stands in (proxies there), a remove to those it left.
        void StreamProxies( const PawnState& pawn, std::uint32_t owner );
        // Performs what the ledger decided: its log lines, its messages, and a moved record's state of record,
        // AuthorityChanged to the gateways and the proxies around the new owner.
        void Apply( Handover::Effects effects );

        MeshEndpoint&                             m_Endpoint;
        NetLogSink                                m_Log;
        InProcessReplicationStore                 m_State;
        NetId                                     m_NextLease = 1;
        // Behind a pointer: HandoverLedger.hpp includes this header (EMeshRole), so the member cannot be by value.
        std::unique_ptr<Handover::Ledger>         m_Ledger;
        std::set<std::pair<NetId, std::uint32_t>> m_RefusedWrites; // (id, sim) whose refused write was logged
        std::map<std::uint32_t, std::set<NetId>>  m_Proxies; // sim -> the pawns it holds as proxies
        MeshAuthorityMap                          m_Map;
    };

    // A replica of the store in a sim or gateway process: reads from the change stream, writes sent to the store.
    // NetIds come from the block the store leased this process (§2M.6). WriteStable returns the replica's version
    // when the value is unchanged and 0 when the write is in flight: the store assigns the version, which arrives
    // on the change stream.
    class RemoteReplicationStore final : public IReplicationStore
    {
    public:
        RemoteReplicationStore( MeshEndpoint& endpoint, NetLogSink log );

        NetId                  AllocateNetId() override;
        void                   Write( const PawnState& state ) override;
        void                   Remove( NetId id ) override;
        std::vector<PawnState> All() const override;
        std::uint32_t          WriteStable( StableNetId id, std::string_view component, std::string_view field,
                                            const ReplicatedValue& value ) override;
        [[nodiscard]] const std::map<StableNetId, std::vector<FieldUpdate>>& StableObjects() const override
        {
            return m_Stable;
        }
        bool                                       DestroyStable( StableNetId id ) override;
        [[nodiscard]] const std::set<StableNetId>& DestroyedStable() const override
        {
            return m_Destroyed;
        }

        // Asks the store for a NetId block (a sim, once its store link is up).
        void RequestLease();
        // The store's messages, routed here by the role's handler; true when @p type was one of them.
        bool               OnStoreMessage( std::uint8_t type, std::span<const std::byte> body );
        [[nodiscard]] bool Synced() const
        {
            return m_Synced;
        }
        [[nodiscard]] bool HasLease() const
        {
            return m_LeaseNext < m_LeaseEnd;
        }
        // In a sim, every pawn the store streams is a proxy (the sim's own writes are never echoed back to it):
        // read-only, the owner's published state. Logged when one enters or leaves the band.
        void TrackProxies()
        {
            m_TrackProxies = true;
        }
        [[nodiscard]] bool IsProxy( NetId id ) const
        {
            return m_Proxies.contains( id );
        }
        // The pawn is this process's own now (promoted) or no longer (released): no proxy, no replica entry.
        void Promote( NetId id )
        {
            m_Proxies.erase( id );
        }
        void Forget( NetId id )
        {
            m_Proxies.erase( id );
            m_Pawns.erase( id );
        }

    private:
        MeshEndpoint&                                   m_Endpoint;
        NetLogSink                                      m_Log;
        std::map<NetId, PawnState>                      m_Pawns;
        std::map<StableNetId, std::vector<FieldUpdate>> m_Stable;
        std::set<StableNetId>                           m_Destroyed;
        NetId                                           m_LeaseNext = 0, m_LeaseEnd = 0;
        bool                                            m_LeaseAsked = false;
        bool                                            m_Synced     = false;
        bool                                            m_TrackProxies = false;
        std::set<NetId>                                 m_Proxies;
    };

    // Gateway -> sim (ISimLink over the mesh): every call goes to the sim the authority map names for the player's
    // pawn; the sim's input acks come back per step.
    class RemoteSimLink final : public ISimLink
    {
    public:
        RemoteSimLink( MeshEndpoint& endpoint, NetLogSink log );

        void PlayerJoined( PlayerId player, std::uint64_t account ) override;
        void PlayerLeft( PlayerId player ) override;
        void PlayerInput( PlayerId player, const InputMessage& input ) override;
        void                                   PlayerView( PlayerId player, const NetVec3& eye ) override;
        [[nodiscard]] std::optional<AckedMove> LastProcessedInput( PlayerId player ) const override;

        // The coordinator's authority map and the sims' acks; true when @p type was a mesh message for this link.
        bool OnMeshMessage( std::uint8_t type, std::span<const std::byte> body );
        // The store's AuthorityChanged of a pawn of this gateway's players: its inputs go to the new owner.
        bool OnStoreMessage( std::uint32_t gateway, std::uint8_t type, std::span<const std::byte> body );
        [[nodiscard]] const std::vector<MeshRegion>& Regions() const
        {
            return m_Map.Regions;
        }

    private:
        [[nodiscard]] std::optional<ConnectionId> Route( PlayerId player, std::string_view what );
        void                                      SendView( PlayerId player, const NetVec3& eye, std::string_view what );

        MeshEndpoint&                                               m_Endpoint;
        NetLogSink                                                  m_Log;
        MeshAuthorityMap                                            m_Map;
        std::map<PlayerId, std::pair<std::uint32_t, std::uint32_t>>
             m_Authority; // player -> (sim, epoch) of its pawn
        std::map<PlayerId, AckedMove> m_Acks;
        // player -> the last eye its client reported; re-sent to the new owner when its pawn's authority moves
        std::map<PlayerId, NetVec3> m_Views;
        std::map<PlayerId, std::pair<std::uint32_t, std::uint32_t>> m_Routed; // player -> (sim, epoch) last logged
    };

    // Sim -> gateway (IGatewayLink over the mesh): spawns go to the gateway that joined the player, destroys to
    // every gateway (a pawn may be relevant to clients of any of them).
    class RemoteGatewayLink final : public IGatewayLink
    {
    public:
        RemoteGatewayLink( MeshEndpoint& endpoint, NetLogSink log );
        void PawnSpawned( PlayerId owner, std::uint32_t player, const PawnState& pawn ) override;
        void PawnDestroyed( NetId id ) override;

        // A gateway's ISimLink calls, applied to @p sim; true when @p type was one of them.
        bool OnGatewayMessage( std::uint32_t gateway, std::uint8_t type, std::span<const std::byte> body,
                               PawnSimulation& sim );
        // After a step: every player's last processed input, to its gateway.
        void SendAcks( const PawnSimulation& sim );
        [[nodiscard]] std::optional<std::uint32_t> GatewayOf( PlayerId player ) const;
        // A handed-over player (the store committed its pawn to @p sim under @p offer): the input steps that
        // queued on the pawn while it was held go to @p sim through the store now, ENDED BY THE OFFER'S
        // END-OF-BACKLOG MARK (EndBacklog, sent also when nothing is forwarded) - the new owner's catch-up stays
        // open until it lands; every later input that still reaches this sim follows (§2M.4). Returns the queued
        // steps forwarded.
        std::size_t ForwardInputs( PlayerId player, std::uint32_t sim, Handover::OfferKey offer,
                                   std::span<const InputStep> queued );
        // The end-of-backlog mark alone: the forward of @p offer to @p sim carried @p forwarded input steps (0
        // when the pawn was gone at the commit - its player left - so the new owner still learns the forward is
        // whole). Sent after the forward's inputs on the same reliable lane through the store, so it lands after
        // them.
        void EndBacklog( PlayerId player, std::uint32_t sim, Handover::OfferKey offer, std::uint32_t forwarded );
        // An adopted player: this sim acks its inputs to @p gateway now.
        void Adopted( PlayerId player, std::uint32_t gateway );

    private:
        MeshEndpoint&                     m_Endpoint;
        NetLogSink                        m_Log;
        std::map<PlayerId, std::uint32_t> m_GatewayOf;
        std::map<PlayerId, std::uint32_t> m_ForwardTo; // player -> the sim that holds its pawn now
    };

    // A sim's PawnSpawned / PawnDestroyed, applied to the gateway process's Gateway; true when @p type was one.
    bool DispatchMeshPawnEvent( std::uint8_t type, std::span<const std::byte> body, IGatewayLink& gateway );
    // An input the store forwards to the pawn's new owner (sent to the old owner after its handover, §2M.4).
    struct MeshForwardedInput
    {
        PlayerId                    Player = 0;
        std::optional<InputMessage> Input; // nullopt: the record did not decode
    };
    // nullopt when @p type is not a ForwardedInput.
    [[nodiscard]] std::optional<MeshForwardedInput> DecodeMeshForwardedInput( std::uint8_t               type,
                                                                              std::span<const std::byte> body );
    // The previous owner's end-of-backlog mark the store relays to the pawn's new owner: the forward of the
    // queued inputs of @p Offer is whole (RemoteGatewayLink::EndBacklog).
    struct MeshBacklogEnd
    {
        PlayerId           Player = 0;
        bool               Decoded = false; // false: the record did not decode (Player may be unset)
        Handover::OfferKey Offer;
        std::uint32_t      Forwarded = 0; // the input steps the forward carried
    };
    // nullopt when @p type is not a ForwardedBacklogEnd.
    [[nodiscard]] std::optional<MeshBacklogEnd> DecodeMeshBacklogEnd( std::uint8_t               type,
                                                                      std::span<const std::byte> body );
    // The mesh clock's step, as every role sees it (nullopt when @p type is not a Tick).
    [[nodiscard]] std::optional<std::uint32_t> DecodeMeshTick( std::uint8_t               type,
                                                               std::span<const std::byte> body );
    // Sim -> coordinator once it holds its NetId lease: it may take players.
    [[nodiscard]] std::vector<std::byte> EncodeMeshSimReady();
    // The coordinator's authority map (nullopt when @p type is not an AuthorityMap).
    [[nodiscard]] std::optional<MeshAuthorityMap> DecodeMeshAuthorityMap( std::uint8_t               type,
                                                                          std::span<const std::byte> body );
    // The coordinator's directory, as a sim or gateway receives it (nullopt when @p type is not a Directory).
    [[nodiscard]] std::optional<std::vector<MeshPeerAddress>>
    DecodeMeshDirectory( std::uint8_t type, std::span<const std::byte> body );
} // namespace Desert::Net

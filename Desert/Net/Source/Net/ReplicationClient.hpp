#pragma once

// THE CLIENT'S SESSION (NET-ARCH §2, §5): connect + Hello, then the replicated pawns this client is told about -
// its own one (Owned) and the other players' - each with an InterpolationBuffer, and the input path to the server.
// Engine-free: the engine's net driver maps net ids to Scene entities (PlayerControllers) and calls
// SampleRemote/SampleOwn per frame; a headless bot (DesertNetProbe --bot) drives the same object.
//
// Input: SendInput(move, yaw) once per client fixed step - the step is numbered, pushed into the InputRing and
// the last kInputRedundancy steps go out under a new sequence id. The server's ack (Snapshot::AckInputStep)
// drops the ring up to it. NET-4 adds the replay from the ack; nothing here corrects a pawn yet.

#include <Net/NetTransport.hpp>
#include <Net/Replication.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

namespace Desert::Net
{
    class ReplicationClient final : public INetTransportEvents
    {
    public:
        enum class EState : std::uint8_t
        {
            Connecting,
            InSession,
            Closed,
        };
        struct RemotePawn
        {
            NetId               Id     = kInvalidNetId;
            std::uint32_t       Player = 0;
            bool                Owned  = false;
            InterpolationBuffer History;
        };

        ReplicationClient( INetTransport& transport, HelloMessage hello, NetLogSink log );

        // Connects to "ip:port"; false + @p error when the address cannot be used.
        bool Connect( std::string_view address, std::string& error );
        // Pumps the transport (call every frame).
        void Poll();
        // One client fixed step of input (call at kNetStepHz while InSession).
        void SendInput( const NetVec3& move, float yaw );

        void OnConnected( ConnectionId connection, std::string_view address ) override;
        void OnClosed( ConnectionId connection, const DisconnectInfo& info ) override;
        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data ) override;

        [[nodiscard]] EState State() const
        {
            return m_State;
        }
        [[nodiscard]] const std::string& CloseReason() const
        {
            return m_CloseReason;
        }
        [[nodiscard]] const std::map<NetId, RemotePawn>& Pawns() const
        {
            return m_Pawns;
        }
        [[nodiscard]] NetId OwnPawn() const
        {
            return m_Own;
        }
        // The server GameMode's pawn prefab (an AssetHandle) from the Welcome: every pawn of this session is one
        // (UE: the replicated pawn actor's class). 0 until welcomed.
        [[nodiscard]] std::uint64_t PawnPrefab() const
        {
            return m_PawnPrefab;
        }
        // The pawn drawn at the interpolation time (estimated server clock - kInterpolationDelaySeconds). The own
        // pawn is drawn the same way until NET-4 predicts it.
        [[nodiscard]] std::optional<PawnState> Sample( NetId id ) const;
        // Spawns / destroys since the last call (the engine creates and releases the entities).
        [[nodiscard]] std::vector<NetId> TakeSpawned();
        [[nodiscard]] std::vector<NetId> TakeDestroyed();
        // STABLE OBJECTS (NET-2 §3.2): the field values the server sent since the last call, newest version only,
        // in arrival order. The engine writes them into the entities its own streamer made from the same records.
        struct StableChange
        {
            StableNetId Id = 0;
            FieldUpdate Field;
        };
        [[nodiscard]] std::vector<StableChange> TakeStableChanges();
        // THE WORLD DELTA (NET-3 §3.3): stable ids the server destroyed, for cells this client reported resident,
        // since the last call. The engine destroys their entities and its streamer skips their records from then.
        [[nodiscard]] std::vector<StableNetId> TakeStableDestroyed();
        // CELL RESIDENCY (§4.3): the cell units that became resident / unloaded here since the last report, on the
        // reliable lane (only changes). @p gone: the stable ids of the unloaded units, whose versions this client
        // forgets so the full state the server sends when they return applies again. Nothing is sent before the
        // session or for an empty change.
        void ReportResidency( const CellResidencyMessage& change, std::span<const StableNetId> gone );
        [[nodiscard]] std::size_t        PendingInputs() const
        {
            return m_Inputs.Size();
        }
        [[nodiscard]] std::optional<std::uint32_t> LastAck() const
        {
            return m_LastAck;
        }

    private:
        using Clock = std::chrono::steady_clock;
        [[nodiscard]] double RenderServerSeconds() const;

        INetTransport&               m_Transport;
        HelloMessage                 m_Hello;
        NetLogSink                   m_Log;
        ConnectionId                 m_Connection = kInvalidConnection;
        std::uint64_t                m_PawnPrefab = 0;
        EState                       m_State      = EState::Connecting;
        std::string                  m_CloseReason;
        std::map<NetId, RemotePawn>  m_Pawns;
        NetId                        m_Own = kInvalidNetId;
        std::vector<NetId>           m_Spawned, m_Destroyed;
        InputRing                    m_Inputs;
        std::uint32_t                m_NextStep = 1, m_NextSequence = 1;
        std::optional<std::uint32_t> m_LastAck;
        StateAckWindow                                               m_StateAcks;
        std::map<std::pair<StableNetId, std::string>, std::uint32_t> m_StableVersions; // newest version per field
        std::vector<StableChange>                                    m_StableChanges;
        std::vector<StableNetId>                                     m_StableDestroyed;
        // The server clock: the newest snapshot's step time and the local time it arrived.
        std::optional<std::uint32_t> m_NewestServerStep;
        Clock::time_point            m_NewestArrival{};
    };
} // namespace Desert::Net

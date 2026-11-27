#pragma once

// THE CLIENT'S SESSION (NET-ARCH §2, §5): connect + Hello, then the replicated pawns this client is told about -
// its own one (Owned) and the other players' - each with an InterpolationBuffer, and the input path to the server.
// Engine-free: the engine's net driver maps net ids to Scene entities (PlayerControllers) and calls
// SampleRemote/SampleOwn per frame; a headless bot (DesertNetProbe --bot) drives the same object.
//
// Input: RecordInput(move, yaw) once per client fixed step - the step is numbered and pushed into the InputRing;
// FlushInputs() once per frame sends every step not acked yet (all of them, up to kMaxUnackedInputSteps) under a
// new sequence id, so a lost packet's steps go again with every later frame until the ack (UE: the client's moves
// batched per net update, each unacked move resent until ClientAckGoodMove). SendInput = one step recorded and
// flushed (a headless bot's frame is one step). The server's ack (Snapshot::AckInputStep) drops the ring up to it;
// the engine's driver replays from it (NET-4).
// At rest (NET-SEND-THROTTLE; UE GetClientNetSendDeltaTime -> ClientNetSendMoveDeltaTimeStationary): while the
// caller says its pawn stands and no step since the last send changed the input, FlushInputs(InputSendPolicy)
// holds the message until StationaryEverySteps steps have gone by - nothing is skipped, the next message carries
// every unacked step - and sends at once on the first changed step or a pawn that moves.

#include <Net/NetTransport.hpp>
#include <Net/Relevancy.hpp>
#include <Net/Replication.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
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
            // WP-FAR-15: the period, in server steps, the server sends this pawn at now (the newest snapshot entry's
            // SnapshotEntry::PeriodSteps), and how much farther behind the connection's delay it is drawn for it:
            // slides toward PawnInterpolationDelay(PeriodSteps) - InterpolationDelay() at the render clock's rate,
            // so a band change never jumps the pawn (UE: a simulated proxy smoothed over its own update interval).
            std::uint8_t PeriodSteps = 2;
            double       ExtraDelay  = 0.0;
        };

        ReplicationClient( INetTransport& transport, HelloMessage hello, NetLogSink log );

        // Connects to "ip:port"; false + @p error when the address cannot be used.
        bool Connect( std::string_view address, std::string& error );
        // Pumps the transport and advances the remote pawns' render clock (call every frame).
        void Poll();
        // One client fixed step of input (call at kNetStepHz while InSession): @p input as the game filled it (move
        // intent and the pawn's declared action set, InputStep), numbered here and snapped to the wire's lattice
        // (InputWire.hpp OnWireLattice) - the step as returned is the input the client's prediction runs and the
        // one the server decodes, the same floats - or nullopt outside the session. It goes out at FlushInputs.
        std::optional<InputStep> RecordInput( InputStep input );
        // Sends every unacked step (all of them, never only the newest) in one Input message; nothing when none is
        // unacked. A run the codec refuses (InputWire.hpp AppendInputRun) is logged and not sent.
        void FlushInputs();
        // What the caller knows of its pawn when it flushes (NET-SEND-THROTTLE): PawnAtRest = the predicted state
        // after the newest recorded step stands (grounded, no speed); StationaryEverySteps = the project's Net
        // StationaryInputEverySteps (validated there, 1..60).
        struct InputSendPolicy
        {
            bool          PawnAtRest           = false;
            std::uint32_t StationaryEverySteps = 1;
        };
        // FlushInputs, except at rest: the message is held while @p policy.PawnAtRest, every step recorded since
        // the last send has the input that send's newest step had (move, yaw, actions, axes), fewer than
        // StationaryEverySteps steps have been recorded since it, and the ring is under half full. Otherwise sent.
        void FlushInputs( const InputSendPolicy& policy );
        // True while the uplink runs at the stationary rate (the last FlushInputs(policy) found the pawn at rest
        // with an unchanged input, whether it held or sent): the server's queue of this client's steps then saws
        // between empty and a held interval and says nothing of the clock sync (ClientNetDriver's dilation).
        [[nodiscard]] bool InputAtStationaryRate() const
        {
            return m_InputAtStationaryRate;
        }
        // THE INPUT UPLINK (UE `stat net` Out bytes, this lane's share): the Input messages' bytes and count over
        // the last whole second of NetClock (0 before one has passed), and the largest message this session with
        // the number of steps it carried, and the message with the most steps (ties: the more bytes) - a starved
        // client's burst; {0, 0} before the first.
        struct InputMessageSize
        {
            std::size_t Bytes = 0;
            std::size_t Steps = 0;
        };
        [[nodiscard]] std::size_t InputBytesPerSecond() const
        {
            return m_InputBytesPerSecond;
        }
        [[nodiscard]] std::size_t InputMessagesPerSecond() const
        {
            return m_InputMessagesPerSecond;
        }
        [[nodiscard]] InputMessageSize LargestInputMessage() const
        {
            return m_LargestInput;
        }
        [[nodiscard]] InputMessageSize LongestInputRun() const
        {
            return m_LongestInput;
        }
        // RecordInput + FlushInputs: a frame of one step.
        std::optional<InputStep> SendInput( InputStep input );
        // THE OWN PAWN'S AUTHORITY (§5.1): the newest ack with the server's state after that step, once per new
        // ack or per newer server step re-acking the same step (a server-side move) (UE ClientAdjustPosition /
        // ClientAckGoodMove), and the server's input queue length with it.
        struct OwnAuthority
        {
            std::uint32_t AckStep = 0;
            PawnMoveState State;
            std::uint8_t  InputBuffered = 0;
        };
        [[nodiscard]] std::optional<OwnAuthority> TakeOwnAuthority()
        {
            return std::exchange( m_OwnAuthority, std::nullopt );
        }

        void OnConnected( ConnectionId connection, std::string_view address ) override;
        void OnClosed( ConnectionId connection, const DisconnectInfo& info ) override;
        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                        NetClock::time_point received ) override;

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
        // The pawn drawn at the interpolation time (the render clock: estimated server clock - InterpolationDelay).
        // The own pawn is drawn the same way until NET-4 predicts it.
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
        // THE VISIBILITY ACKNOWLEDGEMENTS (UE ClientAckUpdateLevelVisibility): cell units the server answered a
        // residency report for, since the last call - each with its world delta in the same message, before it.
        [[nodiscard]] std::vector<std::uint32_t> TakeAcknowledgedUnits();
        // CELL RESIDENCY (§4.3): the cell units that became resident / unloaded here since the last report, on the
        // reliable lane (only changes). @p gone: the stable ids of the unloaded units, whose versions this client
        // forgets so the full state the server sends when they return applies again. Nothing is sent before the
        // session or for an empty change.
        void ReportResidency( const CellResidencyMessage& change, std::span<const StableNetId> gone );
        // WP-FAR-15 THE VIEW (UE APlayerController::ServerUpdateCamera): the point this client's player looks from
        // and the shape it looks through - its view streaming source's - sent on the State lane (unreliable) when
        // the eye moved by kViewPositionStepCm or the shape changed by kViewAngleStepDeg / kViewRangeStepCm (a
        // circle's yaw never counts), or kViewResendSeconds passed, so a lost report is repeated; never more often
        // than kViewMinIntervalSeconds (a teleported eye reaches the server within it plus half the RTT). Nothing
        // before the session.
        void ReportView( const ViewReport& view );
        static constexpr float  kViewPositionStepCm     = 100.0f;
        static constexpr float  kViewAngleStepDeg       = 1.0f;
        static constexpr float  kViewRangeStepCm        = 100.0f;
        static constexpr double kViewResendSeconds      = 1.0;
        static constexpr double kViewMinIntervalSeconds = 0.1;
        // A remote pawn's whole interpolation delay (the connection's + its own, seconds), for the log; nullopt
        // for a pawn this client does not have. Its period is RemotePawn::PeriodSteps.
        [[nodiscard]] std::optional<double> PawnDelay( NetId id ) const;
        [[nodiscard]] std::size_t        PendingInputs() const
        {
            return m_Inputs.Size();
        }
        [[nodiscard]] std::optional<std::uint32_t> LastAck() const
        {
            return m_LastAck;
        }
        // NET-5 (§5.3) THE ADAPTIVE INTERPOLATION DELAY: how far behind the estimated server clock remote pawns are
        // drawn now, and the snapshots' arrival jitter it covers (95th percentile of each arrival's transit above
        // the fastest one over the last kJitterWindowSeconds).
        [[nodiscard]] double InterpolationDelay() const
        {
            return m_Delay;
        }
        [[nodiscard]] double ArrivalJitter() const
        {
            return m_Jitter;
        }
        static constexpr double kJitterWindowSeconds = 2.0;

    private:
        using Clock = NetClock;
        [[nodiscard]] double RenderServerSeconds() const;
        [[nodiscard]] double LocalSeconds() const;
        void                 MeasureArrival( double serverSeconds, NetClock::time_point received );
        void                 AdvanceRenderClock();
        // Closes the input window when a second has passed since it opened (called from Poll and FlushInputs).
        void                 RollInputWindow( double local );
        // The snapshot cadence as received: the median interval between the server times of the snapshots in the
        // jitter window (the server's first band's period; nothing in code says it). Nullopt below two snapshots.
        // Computed once per arrival (MeasureArrival) into m_SnapshotInterval.
        [[nodiscard]] std::optional<double> SnapshotInterval() const;

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
        // The server step of the snapshot whose authority was taken last: a re-ack of m_LastAck from a newer
        // server step is a server-side move (a teleport) and is taken too.
        std::optional<std::uint32_t>                                 m_LastAckServerStep;
        std::optional<OwnAuthority>                                  m_OwnAuthority;
        StateAckWindow                                               m_StateAcks;
        std::map<std::pair<StableNetId, std::string>, std::uint32_t> m_StableVersions; // newest version per field
        std::vector<StableChange>                                    m_StableChanges;
        std::vector<StableNetId>                                     m_StableDestroyed;
        std::vector<std::uint32_t>                                   m_AcknowledgedUnits;
        // The newest snapshot's server step (a spawn's first sample is placed at it).
        std::optional<std::uint32_t> m_NewestServerStep;
        // NET-5: the render clock (Overwatch-style adaptive interpolation). Every snapshot's transit offset (local
        // arrival - server time) over the jitter window; the fastest one maps the server clock to local time, the
        // spread above it is the jitter the delay covers. The render clock runs at local rate dilated by at most
        // kClockDilation towards (estimated server clock - delay), so a changing delay or a jittered estimate
        // never jumps the pawns; it snaps only on a teleport-sized error above kClockSnapSeconds (a new session, a
        // server clock jump). Arrivals are timed at the transport's receive, so a client frame stall moves
        // neither.
        static constexpr double kClockDilation   = 0.1;
        static constexpr double                  kClockSnapSeconds = 2.0;
        Clock::time_point                        m_Epoch = Clock::now();
        std::deque<std::pair<double, double>>    m_Arrivals; // (local seconds, local - server seconds)
        double                                   m_FastestOffset = 0.0, m_Jitter = 0.0;
        std::optional<double>                    m_SnapshotInterval; // SnapshotInterval() at the newest arrival
        double                                   m_Delay         = kInterpolationDelaySeconds;
        std::optional<double>                    m_RenderSeconds;
        double                                   m_LastAdvance = 0.0, m_NextDelayLog = 0.0;
        int                                      m_ClockSnaps = 0; // past the first set, logged
        // The input uplink window (local seconds): what the open window has sent, and the last closed window's totals.
        double           m_InputWindowStart = 0.0;
        std::size_t      m_InputWindowBytes = 0, m_InputWindowMessages = 0;
        std::size_t      m_InputBytesPerSecond = 0, m_InputMessagesPerSecond = 0;
        InputMessageSize m_LargestInput, m_LongestInput;
        // NET-SEND-THROTTLE: the newest step of the last sent Input message (its input is what "unchanged" is
        // measured against), and whether the uplink is at the stationary rate.
        std::optional<InputStep> m_LastSentStep;
        bool                     m_InputAtStationaryRate = false;
        // The first step of the last refused run: a refusal is logged once per run start, not every frame.
        std::optional<std::uint32_t> m_RefusedFrom;
        // The last view (eye and shape) sent (ReportView) and when, in local seconds.
        std::optional<ViewReport>                m_SentView;
        double                                   m_ViewSentAt = 0.0;
    };
} // namespace Desert::Net

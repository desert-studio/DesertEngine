#pragma once

// FaultTransport (NET-SCN-VERBS, NET-SCN-DROP-MSG): a DEVELOPMENT-ONLY DECORATOR (GoF Decorator) over INetTransport
// that drops or truncates CHOSEN messages by their kind, so a scenario proves a refusal path with one console
// line instead of hoping GNS's random FakePacketLoss hits the right packet. Every INetTransport call forwards to
// the inner transport; Send and Poll consult the armed faults. Nothing above the interface knows it is there
// (ReplicationClient takes an INetTransport&), nothing below changes.
//
// THE KIND OF A MESSAGE IS ITS FIRST BYTE: a handshake message (NetHandshake.hpp EHandshakeMessage 1..3) or a
// session message (Replication.hpp EReplicationMessage, kFirstSessionMessage + n; Replication.cpp
// PeekReplicationMessage reads the same byte). A kind flows one way per side (a client sends Input, ViewReport,
// CellResidency, StateAck and receives Snapshot, Spawn, Destroy, ObjectState, StableDestroy), so a fault names no
// direction: it applies wherever that kind passes, Send or Poll.
//
// Drop: the message never reaches the inner transport (Send) or the events (Poll). On a reliable lane that is a
// message lost for good - the transport's reliability sits below this decorator - which is the point: the peer's
// path for a message that never comes is what the scenario asks about.
// Corrupt: the message is cut to its first two bytes (kind + one byte). Every decoder refuses a header shorter
// than its fixed part, and the kind byte survives so the receiver's dispatch names the kind in its refusal line
// ("connection N: malformed Input dropped", MeshOfOne.cpp; "malformed ObjectState dropped",
// ReplicationClient.cpp). Flipping payload bytes would not do: a quantized field decodes to SOME valid value. A
// message shorter than three bytes cannot be cut and is dropped instead (logged as dropped).
//
// LOG LINES (through the NetLogSink given at construction; the owner's sink prefixes its tag, so a bot B logs
// "[B] fault armed: ..."). These are the runner's contract (net_scenario.py «Ждать»): change the text, change
// the scenarios.
//   fault armed: {drop|corrupt} {Kind} x{N}                      Arm / ArmFromConsole
//   fault: dropped {Kind} #{k} of {N} ({Send|Poll}, {bytes} B)   one dropped message
//   fault: corrupted {Kind} #{k} of {N} ({bytes} -> 2 B)        one truncated message
//   fault done: {drop|corrupt} {Kind} x{N}                       the last of the N
//   fault refused: {reason}                                      ArmFromConsole's refusal (also returned)
//
// Cost: one virtual hop and a linear scan of the armed faults (at most one per kind, so <= 12 entries) per
// message; nothing while no fault is armed beyond the empty scan. A shipping process never constructs one.

#include "Net/NetHandshake.hpp"
#include "Net/NetTransport.hpp"
#include "Net/Replication.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    // The console's names for the kinds, the one table (a new message kind is added here and nowhere else in the
    // faults). Handshake kinds first, then the session's.
    struct MessageKindName
    {
        std::uint8_t     Kind;
        std::string_view Name;
    };
    inline constexpr std::array<MessageKindName, 12> kMessageKindNames{ {
         { static_cast<std::uint8_t>( EHandshakeMessage::Hello ), "Hello" },
         { static_cast<std::uint8_t>( EHandshakeMessage::Welcome ), "Welcome" },
         { static_cast<std::uint8_t>( EHandshakeMessage::Refuse ), "Refuse" },
         { static_cast<std::uint8_t>( EReplicationMessage::Input ), "Input" },
         { static_cast<std::uint8_t>( EReplicationMessage::Snapshot ), "Snapshot" },
         { static_cast<std::uint8_t>( EReplicationMessage::Spawn ), "Spawn" },
         { static_cast<std::uint8_t>( EReplicationMessage::Destroy ), "Destroy" },
         { static_cast<std::uint8_t>( EReplicationMessage::ObjectState ), "ObjectState" },
         { static_cast<std::uint8_t>( EReplicationMessage::StateAck ), "StateAck" },
         { static_cast<std::uint8_t>( EReplicationMessage::CellResidency ), "CellResidency" },
         { static_cast<std::uint8_t>( EReplicationMessage::StableDestroy ), "StableDestroy" },
         { static_cast<std::uint8_t>( EReplicationMessage::ViewReport ), "ViewReport" },
    } };

    // The kind named `name` (exact, case-sensitive, as the table spells it); nullopt for any other word.
    [[nodiscard]] std::optional<std::uint8_t> MessageKindByName( std::string_view name );
    // The table's name of `kind`, or "kind N" for a byte the table does not know (a corrupt peer's message).
    [[nodiscard]] std::string MessageKindText( std::uint8_t kind );

    enum class EMessageFault : std::uint8_t
    {
        Drop,
        Corrupt,
    };
    [[nodiscard]] constexpr std::string_view MessageFaultName( EMessageFault fault )
    {
        return fault == EMessageFault::Drop ? "drop" : "corrupt";
    }

    // One armed fault: the next Remaining messages of Kind. Total is what was armed (for the "#k of N" line).
    struct MessageFault
    {
        std::uint8_t  Kind      = 0;
        EMessageFault Fault     = EMessageFault::Drop;
        std::uint32_t Remaining = 0;
        std::uint32_t Total     = 0;
    };

    class FaultTransport final : public INetTransport
    {
    public:
        // `inner` outlives this object (the owner constructs the real transport first and keeps both).
        FaultTransport( INetTransport& inner, NetLogSink log );

        // Arms a fault on `kind`: the next `count` (>= 1) messages of that kind through Send or Poll are dropped
        // or truncated. Replaces an armed fault of the same kind (its remaining count is forgotten, logged as
        // "fault armed" anew). Faults of different kinds coexist.
        void Arm( std::uint8_t kind, EMessageFault fault, std::uint32_t count );
        // One console command: "drop <Kind> [count]" | "corrupt <Kind> [count]", count >= 1, default 1. False +
        // `error` naming the word refused (an unknown verb, an unknown kind, a count that is not >= 1); the
        // refusal is also logged. The owner's console loop hands it every line it does not know itself.
        bool ArmFromConsole( std::string_view command, std::string& error );
        // Every message dropped / truncated so far, for the owner's final line.
        [[nodiscard]] std::uint64_t Dropped() const
        {
            return m_Dropped;
        }
        [[nodiscard]] std::uint64_t Corrupted() const
        {
            return m_Corrupted;
        }
        // The faults still armed (Remaining > 0), for the owner's final line ("N fault(s) never hit").
        [[nodiscard]] const std::vector<MessageFault>& Armed() const
        {
            return m_Faults;
        }

        // INetTransport: forwards; Send applies the faults to outgoing messages, Poll (through Tap) to incoming.
        bool         Listen( std::uint16_t port, std::string& error ) override;
        ConnectionId Connect( std::string_view address, std::string& error ) override;
        ESendResult  Send( ConnectionId connection, std::span<const std::byte> data, ENetLane lane ) override;
        void         Poll( INetTransportEvents& events ) override;
        void         Close( ConnectionId connection, int code, std::string_view reason, bool flush ) override;
        [[nodiscard]] ConnectionStats GetStats( ConnectionId connection ) const override;
        [[nodiscard]] std::string     DescribeConnection( ConnectionId connection ) const override;
        bool SetSimulator( const NetSimulator& simulator, std::string& error ) override;
        bool SetSendRate( int minBytesPerSecond, int maxBytesPerSecond, std::string& error ) override;
        [[nodiscard]] PeerIdentity Peer( ConnectionId connection ) const override;
        bool UseMeshCertificate( const std::string& caDirectory, std::string_view identity,
                                 std::string& error ) override;

    private:
        // What an armed fault did to one message: pass it, drop it, or pass `data` cut to two bytes.
        enum class EVerdict : std::uint8_t
        {
            Pass,
            Drop,
            Truncate,
        };
        // Applies the armed fault of data[0]'s kind, if any: counts, logs the fault line (`way` = "Send" | "Poll",
        // for the dropped line) and returns the verdict. Pass when the message is empty or no fault is armed.
        EVerdict Apply( std::span<const std::byte> data, std::string_view way );

        // The events wrapper Poll hands the inner transport: OnMessage runs Apply, the other two forward.
        class Tap final : public INetTransportEvents
        {
        public:
            Tap( FaultTransport& owner, INetTransportEvents& events )
                : m_Owner( owner )
                , m_Events( events )
            {
            }
            void OnConnected( ConnectionId connection, std::string_view address ) override;
            void OnClosed( ConnectionId connection, const DisconnectInfo& info ) override;
            void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                            NetClock::time_point received ) override;

        private:
            FaultTransport&     m_Owner;
            INetTransportEvents& m_Events;
        };

        INetTransport&            m_Inner;
        NetLogSink                m_Log;
        std::vector<MessageFault> m_Faults; // at most one entry per kind
        std::uint64_t             m_Dropped   = 0;
        std::uint64_t             m_Corrupted = 0;
    };
} // namespace Desert::Net

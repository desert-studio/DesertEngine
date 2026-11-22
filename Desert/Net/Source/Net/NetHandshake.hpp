#pragma once

// The connect handshake (NET-ARCH §8, NET-1): the first message a client sends on a new connection is a Hello with
// the protocol version and the hash of the cooked world it has (the cook hash, Core/Serialize/CookedWorld); the
// server answers Welcome, or Refuse with a reason and closes the connection with code kAppCloseCodeMin + reason.
// A client built against another protocol or holding another cook cannot join: replicated ids are cooked record
// ids (NET-ARCH §3.2), meaningful only between identical cooks.
// Wire format: byte 0 = EHandshakeMessage, then fixed little-endian fields. Session messages after the handshake
// use type bytes >= kFirstSessionMessage.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    // Bumped by every change to any message the protocol carries.
    inline constexpr std::uint32_t kProtocolVersion = 1;
    // "DSNT": a Hello from something that is not our client is refused before its fields are read.
    inline constexpr std::uint32_t kHelloMagic = 0x544E5344u;

    enum class EHandshakeMessage : std::uint8_t
    {
        Hello = 1,
        Welcome = 2,
        Refuse = 3,
    };
    inline constexpr std::uint8_t kFirstSessionMessage = 16;

    enum class ERefuseReason : std::uint8_t
    {
        NotAHello = 1,       // the first message was not a well-formed Hello
        ProtocolVersion = 2, // client and server speak different protocol versions
        CookHash = 3,        // client and server have different cooked worlds
    };
    // The one text of each reason, in both logs and in the close reason the client receives.
    std::string_view RefuseReasonText(ERefuseReason reason);
    // Close code for a refusal (INetTransport::Close / DisconnectInfo::Code).
    int RefuseCloseCode(ERefuseReason reason);

    struct HelloMessage
    {
        std::uint32_t ProtocolVersion = 0;
        std::uint64_t CookHash = 0;
    };

    std::vector<std::byte> EncodeHello(const HelloMessage& hello);
    std::vector<std::byte> EncodeWelcome(std::uint64_t cookHash);
    std::vector<std::byte> EncodeRefuse(ERefuseReason reason);

    // The server's decision on the first message of a connection.
    struct HelloVerdict
    {
        bool Accepted = false;
        ERefuseReason Reason = ERefuseReason::NotAHello; // meaningful when !Accepted
        HelloMessage Hello;                              // as received; zero when the message was not a Hello
    };
    HelloVerdict EvaluateHello(std::span<const std::byte> message, std::uint64_t serverCookHash);

    // The client's reading of the server's answer to its Hello.
    struct HandshakeAnswer
    {
        enum class EKind : std::uint8_t
        {
            Welcome,
            Refuse,
            Malformed,
        };
        EKind Kind = EKind::Malformed;
        ERefuseReason Reason = ERefuseReason::NotAHello; // meaningful for Refuse
        std::uint64_t ServerCookHash = 0;                // meaningful for Welcome
    };
    HandshakeAnswer DecodeAnswer(std::span<const std::byte> message);
} // namespace Desert::Net

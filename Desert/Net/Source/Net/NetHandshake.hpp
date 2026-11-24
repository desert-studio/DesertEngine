#pragma once

// The connect handshake (NET-ARCH §8, NET-1): the first message a client sends on a new connection (on the Control
// lane) is a Hello with the protocol version, the hash of the cooked world it has (the cook hash,
// Core/Serialize/CookHash.hpp) and its connect token (ConnectToken.hpp); the server answers Welcome, or Refuse
// with a reason and closes the connection with code kAppCloseCodeMin + reason. Checked in that order: the protocol
// (a client of another version may lay out the rest differently), the token (nothing else is answered to a client
// we did not admit), the cook. A client built against another protocol or holding another cook cannot join:
// replicated ids are cooked record ids (NET-ARCH §3.2), meaningful only between identical cooks. Wire format: byte
// 0 = EHandshakeMessage, then fixed little-endian fields. Session messages after the handshake use type bytes >=
// kFirstSessionMessage.

#include "Net/ConnectToken.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    // Bumped by every change to any message the protocol carries.
    inline constexpr std::uint32_t kProtocolVersion =
         4; // 2: the connect token in the Hello; 3: the pawn in the Welcome; 4: pawn velocity/movement/teleport
    // "DSNT": a Hello from something that is not our client is refused before its fields are read.
    inline constexpr std::uint32_t kHelloMagic = 0x544E5344u;

    enum class EHandshakeMessage : std::uint8_t
    {
        Hello   = 1,
        Welcome = 2,
        Refuse  = 3,
    };
    inline constexpr std::uint8_t kFirstSessionMessage = 16;

    enum class ERefuseReason : std::uint8_t
    {
        NotAHello       = 1, // the first message was not a well-formed Hello
        ProtocolVersion = 2, // client and server speak different protocol versions
        CookHash        = 3, // client and server have different cooked worlds
        TokenExpired    = 4, // the connect token is ours but past its expiry
        TokenInvalid    = 5, // the connect token is malformed, not signed by our secret, or for another server
    };
    // The one text of each reason, in both logs and in the close reason the client receives.
    std::string_view RefuseReasonText( ERefuseReason reason );
    // Close code for a refusal (INetTransport::Close / DisconnectInfo::Code).
    int RefuseCloseCode( ERefuseReason reason );

    struct HelloMessage
    {
        std::uint32_t ProtocolVersion = 0;
        std::uint64_t CookHash        = 0;
        // The token's wire bytes (IssueConnectToken), carried as they are: the server verifies, the client only
        // forwards what the login backend gave it.
        std::vector<std::byte> Token;
    };

    std::vector<std::byte> EncodeHello( const HelloMessage& hello );
    // The Welcome names the pawn this server's GameMode spawns for every player (UE: AGameModeBase
    // DefaultPawnClass, replicated as the pawn actor's class): the asset handle of its prefab, the same number on
    // every machine (Common::AssetHandle::FromKey of the prefab's stable key). The level a client has open need
    // not name a pawn at all.
    std::vector<std::byte> EncodeWelcome( std::uint64_t cookHash, std::uint64_t pawnPrefab );
    std::vector<std::byte> EncodeRefuse( ERefuseReason reason );

    // What a server admits: its cook, its id in the tokens, the secret its tokens are signed with, and the time.
    struct HelloPolicy
    {
        std::uint64_t              CookHash = 0;
        std::uint64_t              ServerId = 0;
        std::span<const std::byte> TokenSecret;
        std::uint64_t              NowUnixSeconds = 0;
    };

    // The server's decision on the first message of a connection.
    struct HelloVerdict
    {
        bool          Accepted = false;
        ERefuseReason Reason   = ERefuseReason::NotAHello; // meaningful when !Accepted
        HelloMessage  Hello;                               // as received; zero when the message was not a Hello
        ConnectToken  Token;                               // as signed; zero unless the signature verified
    };
    HelloVerdict EvaluateHello( std::span<const std::byte> message, const HelloPolicy& policy );

    // The client's reading of the server's answer to its Hello.
    struct HandshakeAnswer
    {
        enum class EKind : std::uint8_t
        {
            Welcome,
            Refuse,
            Malformed,
        };
        EKind         Kind           = EKind::Malformed;
        ERefuseReason Reason         = ERefuseReason::NotAHello; // meaningful for Refuse
        std::uint64_t ServerCookHash = 0;                        // meaningful for Welcome
        std::uint64_t PawnPrefab     = 0;                        // meaningful for Welcome, never 0 there
    };
    HandshakeAnswer DecodeAnswer( std::span<const std::byte> message );
} // namespace Desert::Net

#pragma once

// The connect handshake (NET-ARCH §8, NET-1): the first message a client sends on a new connection (on the Control
// lane) is a Hello with the protocol version, the hash of the cooked world it has (the cook hash,
// Core/Serialize/CookHash.hpp), its connect token (ConnectToken.hpp) and its join options (the game's, unsigned);
// the server answers Welcome, or Refuse with a reason and its text and closes the connection with code
// kAppCloseCodeMin + reason. Checked in that order: the protocol (a client of another version may lay out the
// rest differently), the token (nothing else is answered to a client we did not admit), the cook; then the
// game's PreLogin (PreLogin.hpp) decides on the token's claims. A client built against another protocol or holding
// another cook cannot join: replicated ids are cooked record ids (NET-ARCH §3.2), meaningful only between
// identical cooks. Wire format: byte 0 = EHandshakeMessage, then fixed little-endian fields. Session messages
// after the handshake use type bytes >= kFirstSessionMessage.

#include "Net/ConnectToken.hpp"
#include "Net/PreLogin.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    // Bumped by every change to any message the protocol carries.
    // 2: the connect token in the Hello; 3: the pawn in the Welcome; 4: pawn velocity/movement/teleport; 5: the
    // signed token envelope (Ed25519, claims), join options in the Hello, the refusal's text in the Refuse.
    inline constexpr std::uint32_t kProtocolVersion = 5;
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
        NotAHello       = 1,  // the first message was not a well-formed Hello
        ProtocolVersion = 2,  // client and server speak different protocol versions
        CookHash        = 3,  // client and server have different cooked worlds
        TokenExpired    = 4,  // ETokenCheck::Expired
        TokenMalformed  = 5,  // ETokenCheck::Malformed
        TokenUnknownKey = 6,  // ETokenCheck::UnknownKey
        TokenSignature  = 7,  // ETokenCheck::Signature
        TokenAudience   = 8,  // ETokenCheck::Audience
        TokenLifetime   = 9,  // ETokenCheck::Lifetime
        TokenReplayed   = 10, // ETokenCheck::Replayed
        PreLogin        = 11, // the game's PreLogin refused; the Refuse carries its reason text
    };
    // The one text of each reason, in both logs and in the close reason the client receives (for PreLogin the
    // game's own text replaces it).
    std::string_view RefuseReasonText( ERefuseReason reason );
    // The refusal for a token check that is not Valid.
    ERefuseReason RefuseReasonOf( ETokenCheck check );
    // Close code for a refusal (INetTransport::Close / DisconnectInfo::Code).
    int RefuseCloseCode( ERefuseReason reason );

    struct HelloMessage
    {
        std::uint32_t ProtocolVersion = 0;
        std::uint64_t CookHash        = 0;
        // The token's wire bytes (MintConnectToken), carried as they are: the server verifies, the client only
        // forwards what the login backend gave it.
        std::vector<std::byte> Token;
        // The game's join options (UE: the login URL's options, e.g. "?name=Bot"): the client's own, unsigned;
        // only the game's PreLogin reads them. At most kMaxJoinOptions bytes.
        std::string JoinOptions;
    };
    inline constexpr std::size_t kMaxJoinOptions = 1024;

    std::vector<std::byte> EncodeHello( const HelloMessage& hello );
    // The Welcome names the pawn this server's GameMode spawns for every player (UE: AGameModeBase
    // DefaultPawnClass, replicated as the pawn actor's class): the asset handle of its prefab, the same number on
    // every machine (Common::AssetHandle::FromKey of the prefab's stable key). The level a client has open need
    // not name a pawn at all.
    std::vector<std::byte> EncodeWelcome( std::uint64_t cookHash, std::uint64_t pawnPrefab );
    // @p text: what the client is told (RefuseReasonText, or the game's PreLogin reason), cut at kMaxRefusalText.
    std::vector<std::byte> EncodeRefuse( ERefuseReason reason, std::string_view text );

    // What a server admits: its cook and the time (the tokens are the ConnectTokenVerifier's).
    struct HelloPolicy
    {
        std::uint64_t CookHash       = 0;
        std::uint64_t NowUnixSeconds = 0;
    };

    // The server's decision on the first message of a connection.
    struct HelloVerdict
    {
        bool          Accepted = false;
        ERefuseReason Reason   = ERefuseReason::NotAHello; // meaningful when !Accepted
        HelloMessage  Hello;                               // as received; zero when the message was not a Hello
        ConnectToken  Token;                               // as signed; empty unless the signature verified
    };
    // Protocol, token (@p tokens records the nonce of a valid one), cook. The game's PreLogin comes after.
    HelloVerdict EvaluateHello( std::span<const std::byte> message, const HelloPolicy& policy,
                                ConnectTokenVerifier& tokens );

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
        std::string   ReasonText;                                // meaningful for Refuse: what the server said
        std::uint64_t ServerCookHash = 0;                        // meaningful for Welcome
        std::uint64_t PawnPrefab     = 0;                        // meaningful for Welcome, never 0 there
    };
    HandshakeAnswer DecodeAnswer( std::span<const std::byte> message );
} // namespace Desert::Net

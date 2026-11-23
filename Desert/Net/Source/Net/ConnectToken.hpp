#pragma once

// THE CONNECT TOKEN (NET-ARCH §8; the yojimbo idea, UE: the login URL's auth options checked in PreLogin before a
// PlayerController exists): our login backend signs {account, server, expiry} with the server's secret; the client
// carries the token in its Hello; the server checks it offline - no password and no backend round trip ever reach
// a game server. Signature: HMAC-SHA256 (OpenSSL on Linux/macOS, BCrypt on Windows - the crypto GNS already
// links). Wire format (kConnectTokenSize bytes, little-endian): u8 format, u64 account, u64 server, u64 expiry
// (Unix seconds), 32-byte HMAC-SHA256 of everything before it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::Net
{
    inline constexpr std::uint8_t kConnectTokenFormat  = 1;
    inline constexpr std::size_t  kConnectTokenMacSize = 32;
    inline constexpr std::size_t  kConnectTokenSize    = 1 + 8 + 8 + 8 + kConnectTokenMacSize;
    // The shortest server secret accepted: HMAC-SHA256 keys shorter than the hash weaken it (RFC 2104 §3).
    inline constexpr std::size_t kMinTokenSecretSize = 32;

    struct ConnectToken
    {
        std::uint64_t Account            = 0;
        std::uint64_t Server             = 0; // the server (world gateway) id the token admits to
        std::uint64_t ExpiresUnixSeconds = 0;
    };

    // The issuer (the login backend; DesertNetProbe --issue-token in development): the token's wire bytes, or an
    // empty vector + `error` when the secret is shorter than kMinTokenSecretSize or the platform crypto failed.
    std::vector<std::byte> IssueConnectToken( const ConnectToken& token, std::span<const std::byte> secret,
                                              std::string& error );

    enum class ETokenCheck : std::uint8_t
    {
        Valid,
        Expired, // signed by our secret for this server, but past its expiry
        Invalid, // wrong size or format, a signature that is not ours, or a token for another server
    };
    struct TokenVerdict
    {
        ETokenCheck  Check = ETokenCheck::Invalid;
        ConnectToken Token; // as signed; zero unless the signature verified
    };
    // The server's check: the signature first (constant time), so an unsigned token's fields are never trusted;
    // then the server id, then the expiry against @p nowUnixSeconds.
    TokenVerdict VerifyConnectToken( std::span<const std::byte> token, std::span<const std::byte> secret,
                                     std::uint64_t serverId, std::uint64_t nowUnixSeconds );

    // The current time in the token's unit.
    std::uint64_t UnixSecondsNow();

    // The server's token secret from a text file holding it as hex (the login backend holds the same key; NET-8
    // replaces the file with the backend's key rotation). Empty + @p error when unreadable, not hex or shorter
    // than kMinTokenSecretSize.
    std::vector<std::byte> ReadTokenSecretFile( const std::string& path, std::string& error );
} // namespace Desert::Net

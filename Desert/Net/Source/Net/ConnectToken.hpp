#pragma once

// THE CONNECT TOKEN (NET-ARCH §8; netcode.io / yojimbo connect token, EOS JWT): a SIGNED ENVELOPE the login
// backend (the issuer) gives a client for one world; the client carries it in its Hello; every server of that
// world checks it offline with the issuer's PUBLIC key - no password, no backend round trip and no signing secret
// ever reach a game server. The envelope is all Net reads: version, key id, issuer, audience (the world / cluster
// id), issue and expiry times, a random nonce (a token is admitted once per server), and the CLAIMS - opaque bytes
// the issuer signed for the game, which Net never parses: admission rules (an account banned, a world full, a role
// required) are the game's PreLogin (Net/PreLogin.hpp; UE AGameModeBase::PreLogin), not Net's. Signature: Ed25519
// (the 25519 crypto GameNetworkingSockets already builds: OpenSSL on Linux/macOS, donna on Windows). Wire format
// (little-endian): u8 version, u32 key id, u64 issuer, u64 audience, u64 issued (Unix s), u64 expires (Unix s),
// 16-byte nonce, u16 claims size, the claims, 64-byte Ed25519 signature of everything before.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    inline constexpr std::uint8_t kConnectTokenVersion = 2;  // 1: HMAC-SHA256 under a secret every server held
    inline constexpr std::size_t  kIssuerKeySize       = 32; // an Ed25519 public key or private seed
    inline constexpr std::size_t  kConnectTokenSignatureSize = 64;
    inline constexpr std::size_t  kConnectTokenNonceSize     = 16;
    inline constexpr std::size_t  kMaxConnectTokenClaims     = 1024;
    // The longest a token may be valid (expires - issued): a server keeps each admitted nonce until its token
    // expires, so this bounds the replay cache. A token admits a JOIN, not a session: the session outlives it.
    inline constexpr std::uint64_t kMaxConnectTokenLifetimeSeconds = 24 * 3600;

    using TokenNonce = std::array<std::byte, kConnectTokenNonceSize>;
    using IssuerKey  = std::array<std::byte, kIssuerKeySize>;

    // The envelope as the issuer signed it.
    struct ConnectToken
    {
        std::uint32_t          KeyId              = 0; // which of the issuer's keys signed it (key rotation)
        std::uint64_t          Issuer             = 0; // the login backend
        std::uint64_t          Audience           = 0; // the world (cluster) id the token admits to
        std::uint64_t          IssuedUnixSeconds  = 0;
        std::uint64_t          ExpiresUnixSeconds = 0;
        TokenNonce             Nonce{};
        std::vector<std::byte> Claims; // opaque to Net: the game's PreLogin reads them
    };

    // An issuer key: the PUBLIC half is what servers trust; the PRIVATE half (the Ed25519 seed) exists only in the
    // issuer - the login backend, DesertNetProbe --mint-token in development - never in DesertServer.
    struct IssuerKeyRecord
    {
        std::uint64_t Issuer = 0;
        std::uint32_t KeyId  = 0;
        IssuerKey     Key{};
    };

    // A new issuer key pair; false + @p error when the platform crypto failed.
    bool GenerateIssuerKeys( std::uint64_t issuer, std::uint32_t keyId, IssuerKeyRecord& privateKey,
                             IssuerKeyRecord& publicKey, std::string& error );

    // The issuer: the token's wire bytes with a fresh random nonce and the key's issuer and key id (the fields of
    // @p token carrying them are ignored), or empty + @p error when the claims are larger than
    // kMaxConnectTokenClaims or the key is not an Ed25519 seed.
    std::vector<std::byte> MintConnectToken( const ConnectToken& token, const IssuerKeyRecord& privateKey,
                                             std::string& error );

    enum class ETokenCheck : std::uint8_t
    {
        Valid,
        Malformed,  // wrong size, unknown version, claims larger than allowed
        UnknownKey, // no trusted public key for its issuer and key id
        Signature,  // the signature does not verify under that key: forged or altered in transit
        Audience,   // signed for another world
        Expired,    // past its expiry
        Lifetime,   // valid for longer than kMaxConnectTokenLifetimeSeconds
        Replayed,   // its nonce was already admitted by this server
    };
    // The one text of each check, in the server log and the refusal the client receives.
    std::string_view TokenCheckText( ETokenCheck check );

    struct TokenVerdict
    {
        ETokenCheck  Check = ETokenCheck::Malformed;
        ConnectToken Token; // as signed; empty unless the signature verified
    };

    // THE SERVER'S CHECK: the issuer public keys it trusts and the world it serves; a token's fields are read only
    // after its signature verified. Holds the replay cache: a valid token's nonce is recorded until the token
    // expires, and a second use of it is refused (netcode.io's connect token entries).
    class ConnectTokenVerifier
    {
    public:
        ConnectTokenVerifier( std::vector<IssuerKeyRecord> trustedKeys, std::uint64_t audience );

        // Signature, audience, expiry, lifetime, then the nonce, against @p nowUnixSeconds.
        TokenVerdict Verify( std::span<const std::byte> token, std::uint64_t nowUnixSeconds );

        [[nodiscard]] std::uint64_t Audience() const
        {
            return m_Audience;
        }
        [[nodiscard]] std::size_t RememberedNonces() const
        {
            return m_Admitted.size();
        }

    private:
        std::vector<IssuerKeyRecord>        m_Trusted;
        std::uint64_t                       m_Audience = 0;
        std::map<TokenNonce, std::uint64_t> m_Admitted; // nonce -> the expiry it is remembered until
    };

    // The current time in the token's unit.
    std::uint64_t UnixSecondsNow();

    // An issuer key file: one line "<issuer> <key id> <key as hex>" (the public file for servers, the private one
    // for the issuer). Read: nullopt + @p error when unreadable or malformed. Write: false + @p error.
    std::optional<IssuerKeyRecord> ReadIssuerKeyFile( const std::string& path, std::string& error );
    bool WriteIssuerKeyFile( const std::string& path, const IssuerKeyRecord& key, std::string& error );
} // namespace Desert::Net

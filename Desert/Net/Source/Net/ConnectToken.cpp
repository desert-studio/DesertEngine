#include "Net/ConnectToken.hpp"

#include "steamnetworkingsockets/steamnetworkingsockets_internal.h"
#include "crypto.h"
#include "crypto_25519.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

namespace Desert::Net
{
    namespace
    {
        // Field offsets: version 0, key id 1, issuer 5, audience 13, issued 21, expires 29, nonce 37, claims size
        // 53, claims 55.
        constexpr std::size_t kNonceOffset = 37;
        constexpr std::size_t kHeaderSize  = kNonceOffset + kConnectTokenNonceSize + 2;
        static_assert( kConnectTokenSignatureSize == k_cubCryptoSignature );

        void Put( std::vector<std::byte>& out, std::uint64_t value, int bytes )
        {
            for ( int i = 0; i < bytes; ++i )
            {
                out.push_back( static_cast<std::byte>( ( value >> ( 8 * i ) ) & 0xFFu ) );
            }
        }

        std::uint64_t Get( std::span<const std::byte> in, std::size_t offset, int bytes )
        {
            std::uint64_t value = 0;
            for ( int i = 0; i < bytes; ++i )
            {
                value |= static_cast<std::uint64_t>( in[offset + i] ) << ( 8 * i );
            }
            return value;
        }

        std::string ToHex( std::span<const std::byte> bytes )
        {
            std::string text;
            text.reserve( bytes.size() * 2 );
            for ( const std::byte b : bytes )
            {
                text += std::format( "{:02x}", static_cast<unsigned>( b ) );
            }
            return text;
        }

        bool FromHex( std::string_view text, std::span<std::byte> out )
        {
            const auto nibble = []( char c ) -> int
            {
                if ( c >= '0' && c <= '9' )
                    return c - '0';
                if ( c >= 'a' && c <= 'f' )
                    return c - 'a' + 10;
                if ( c >= 'A' && c <= 'F' )
                    return c - 'A' + 10;
                return -1;
            };
            if ( text.size() != out.size() * 2 )
            {
                return false;
            }
            for ( std::size_t i = 0; i < out.size(); ++i )
            {
                const int hi = nibble( text[2 * i] ), lo = nibble( text[2 * i + 1] );
                if ( hi < 0 || lo < 0 )
                {
                    return false;
                }
                out[i] = static_cast<std::byte>( ( hi << 4 ) | lo );
            }
            return true;
        }
    } // namespace

    bool GenerateIssuerKeys( std::uint64_t issuer, std::uint32_t keyId, IssuerKeyRecord& privateKey,
                             IssuerKeyRecord& publicKey, std::string& error )
    {
        CCrypto::Init(); // the issuer runs no transport: GameNetworkingSockets_Init does not do it here
        CECSigningPublicKey  publicHalf;
        CECSigningPrivateKey privateHalf;
        CCrypto::GenerateSigningKeyPair( &publicHalf, &privateHalf );
        if ( publicHalf.GetRawData( nullptr ) != kIssuerKeySize ||
             privateHalf.GetRawData( nullptr ) != kIssuerKeySize )
        {
            error = "Ed25519 key generation failed in the platform crypto";
            return false;
        }
        publicHalf.GetRawData( publicKey.Key.data() );
        privateHalf.GetRawData( privateKey.Key.data() );
        privateKey.Issuer = publicKey.Issuer = issuer;
        privateKey.KeyId = publicKey.KeyId = keyId;
        return true;
    }

    std::vector<std::byte> MintConnectToken( const ConnectToken& token, const IssuerKeyRecord& privateKey,
                                             std::string& error )
    {
        if ( token.Claims.size() > kMaxConnectTokenClaims )
        {
            error = std::format( "claims are {} bytes; at most {} are carried", token.Claims.size(),
                                 kMaxConnectTokenClaims );
            return {};
        }
        CCrypto::Init();
        CECSigningPrivateKey key;
        if ( !key.SetRawDataWithoutWipingInput( privateKey.Key.data(), privateKey.Key.size() ) )
        {
            error = "the issuer private key is not an Ed25519 seed";
            return {};
        }
        TokenNonce nonce;
        CCrypto::GenerateRandomBlock( nonce.data(), static_cast<int>( nonce.size() ) );

        std::vector<std::byte> out;
        out.reserve( kHeaderSize + token.Claims.size() + kConnectTokenSignatureSize );
        Put( out, kConnectTokenVersion, 1 );
        Put( out, privateKey.KeyId, 4 );
        Put( out, privateKey.Issuer, 8 );
        Put( out, token.Audience, 8 );
        Put( out, token.IssuedUnixSeconds, 8 );
        Put( out, token.ExpiresUnixSeconds, 8 );
        out.insert( out.end(), nonce.begin(), nonce.end() );
        Put( out, token.Claims.size(), 2 );
        out.insert( out.end(), token.Claims.begin(), token.Claims.end() );
        CryptoSignature_t signature;
        key.GenerateSignature( out.data(), out.size(), &signature );
        const auto* signatureBytes = reinterpret_cast<const std::byte*>( signature );
        out.insert( out.end(), signatureBytes, signatureBytes + kConnectTokenSignatureSize );
        return out;
    }

    std::string_view TokenCheckText( ETokenCheck check )
    {
        switch ( check )
        {
            case ETokenCheck::Valid:
                return "token valid";
            case ETokenCheck::Malformed:
                return "token malformed";
            case ETokenCheck::UnknownKey:
                return "token issuer key not trusted";
            case ETokenCheck::Signature:
                return "token signature invalid";
            case ETokenCheck::Audience:
                return "token is for another world";
            case ETokenCheck::Expired:
                return "token expired";
            case ETokenCheck::Lifetime:
                return "token lifetime too long";
            case ETokenCheck::Replayed:
                return "token already used";
        }
        return "token unknown check";
    }

    ConnectTokenVerifier::ConnectTokenVerifier( std::vector<IssuerKeyRecord> trustedKeys, std::uint64_t audience )
         : m_Trusted( std::move( trustedKeys ) ), m_Audience( audience )
    {
        CCrypto::Init();
    }

    TokenVerdict ConnectTokenVerifier::Verify( std::span<const std::byte> token, std::uint64_t nowUnixSeconds )
    {
        TokenVerdict verdict;
        if ( token.size() < kHeaderSize + kConnectTokenSignatureSize ||
             token[0] != static_cast<std::byte>( kConnectTokenVersion ) )
        {
            return verdict;
        }
        const std::size_t claimsSize = static_cast<std::size_t>( Get( token, kHeaderSize - 2, 2 ) );
        const std::size_t signedSize = kHeaderSize + claimsSize;
        if ( claimsSize > kMaxConnectTokenClaims || token.size() != signedSize + kConnectTokenSignatureSize )
        {
            return verdict;
        }
        const auto keyId   = static_cast<std::uint32_t>( Get( token, 1, 4 ) );
        const auto issuer  = Get( token, 5, 8 );
        const auto trusted = std::find_if( m_Trusted.begin(), m_Trusted.end(), [&]( const IssuerKeyRecord& key )
                                           { return key.Issuer == issuer && key.KeyId == keyId; } );
        if ( trusted == m_Trusted.end() )
        {
            verdict.Check = ETokenCheck::UnknownKey;
            return verdict;
        }
        CECSigningPublicKey publicKey;
        CryptoSignature_t   signature;
        std::memcpy( signature, token.data() + signedSize, kConnectTokenSignatureSize );
        if ( !publicKey.SetRawDataWithoutWipingInput( trusted->Key.data(), trusted->Key.size() ) ||
             !publicKey.VerifySignature( token.data(), signedSize, signature ) )
        {
            verdict.Check = ETokenCheck::Signature;
            return verdict;
        }
        // Signed by a key we trust: its fields may now be read.
        ConnectToken& fields      = verdict.Token;
        fields.KeyId              = keyId;
        fields.Issuer             = issuer;
        fields.Audience           = Get( token, 13, 8 );
        fields.IssuedUnixSeconds  = Get( token, 21, 8 );
        fields.ExpiresUnixSeconds = Get( token, 29, 8 );
        std::memcpy( fields.Nonce.data(), token.data() + kNonceOffset, kConnectTokenNonceSize );
        fields.Claims.assign( token.begin() + kHeaderSize, token.begin() + signedSize );
        if ( fields.Audience != m_Audience )
            verdict.Check = ETokenCheck::Audience;
        else if ( fields.ExpiresUnixSeconds <= nowUnixSeconds )
            verdict.Check = ETokenCheck::Expired;
        else if ( fields.ExpiresUnixSeconds > fields.IssuedUnixSeconds &&
                  fields.ExpiresUnixSeconds - fields.IssuedUnixSeconds > kMaxConnectTokenLifetimeSeconds )
            verdict.Check = ETokenCheck::Lifetime;
        else
        {
            // The nonces of expired tokens are forgotten: such a token is refused as Expired before this.
            std::erase_if( m_Admitted,
                           [nowUnixSeconds]( const auto& entry ) { return entry.second <= nowUnixSeconds; } );
            verdict.Check = m_Admitted.emplace( fields.Nonce, fields.ExpiresUnixSeconds ).second
                                 ? ETokenCheck::Valid
                                 : ETokenCheck::Replayed;
        }
        return verdict;
    }

    std::uint64_t UnixSecondsNow()
    {
        return static_cast<std::uint64_t>( std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch() )
                                                .count() );
    }

    std::optional<IssuerKeyRecord> ReadIssuerKeyFile( const std::string& path, std::string& error )
    {
        std::ifstream   file( path );
        IssuerKeyRecord key;
        std::string     hex;
        if ( !file || !( file >> key.Issuer >> key.KeyId >> hex ) || !FromHex( hex, key.Key ) )
        {
            error =
                 std::format( "issuer key file '{}' must hold one line '<issuer> <key id> <{}-byte key as hex>'",
                              path, kIssuerKeySize );
            return std::nullopt;
        }
        return key;
    }

    bool WriteIssuerKeyFile( const std::string& path, const IssuerKeyRecord& key, std::string& error )
    {
        {
            std::ofstream file( path, std::ios::trunc );
            if ( !( file << std::format( "{} {} {}\n", key.Issuer, key.KeyId, ToHex( key.Key ) ) ) )
            {
                error = std::format( "cannot write issuer key file '{}'", path );
                return false;
            }
        }
        // A private key readable by others is a key anyone on the machine can mint with.
        std::error_code failure;
        std::filesystem::permissions(
             path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, failure );
        if ( failure )
        {
            error =
                 std::format( "cannot restrict issuer key file '{}' to its owner: {}", path, failure.message() );
            return false;
        }
        return true;
    }
} // namespace Desert::Net

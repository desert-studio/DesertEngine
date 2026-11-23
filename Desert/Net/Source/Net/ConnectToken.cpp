#include "Net/ConnectToken.hpp"

#include <chrono>
#include <format>

#if defined( _WIN32 )
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace Desert::Net
{
    namespace
    {
        constexpr std::size_t kSignedSize = kConnectTokenSize - kConnectTokenMacSize;
        using Mac                         = std::array<std::byte, kConnectTokenMacSize>;

        void Put( std::vector<std::byte>& out, std::uint64_t value, int bytes )
        {
            for ( int i = 0; i < bytes; ++i )
            {
                out.push_back( static_cast<std::byte>( ( value >> ( 8 * i ) ) & 0xFFu ) );
            }
        }

        std::uint64_t Get( std::span<const std::byte> in, std::size_t offset )
        {
            std::uint64_t value = 0;
            for ( int i = 0; i < 8; ++i )
            {
                value |= static_cast<std::uint64_t>( in[offset + i] ) << ( 8 * i );
            }
            return value;
        }

        // HMAC-SHA256 of @p data under @p key; false when the platform crypto fails.
        bool HmacSha256( std::span<const std::byte> key, std::span<const std::byte> data, Mac& out )
        {
#if defined( _WIN32 )
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            if ( !BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                               BCRYPT_ALG_HANDLE_HMAC_FLAG ) ) )
            {
                return false;
            }
            BCRYPT_HASH_HANDLE hash = nullptr;
            bool               ok   = BCRYPT_SUCCESS( BCryptCreateHash(
                 algorithm, &hash, nullptr, 0, reinterpret_cast<PUCHAR>( const_cast<std::byte*>( key.data() ) ),
                 static_cast<ULONG>( key.size() ), 0 ) );
            ok                      = ok && BCRYPT_SUCCESS( BCryptHashData(
                            hash, reinterpret_cast<PUCHAR>( const_cast<std::byte*>( data.data() ) ),
                            static_cast<ULONG>( data.size() ), 0 ) );
            ok = ok && BCRYPT_SUCCESS( BCryptFinishHash( hash, reinterpret_cast<PUCHAR>( out.data() ),
                                                         static_cast<ULONG>( out.size() ), 0 ) );
            if ( hash )
            {
                BCryptDestroyHash( hash );
            }
            BCryptCloseAlgorithmProvider( algorithm, 0 );
            return ok;
#else
            unsigned int length = 0;
            return HMAC( EVP_sha256(), key.data(), static_cast<int>( key.size() ),
                         reinterpret_cast<const unsigned char*>( data.data() ), data.size(),
                         reinterpret_cast<unsigned char*>( out.data() ), &length ) != nullptr &&
                   length == out.size();
#endif
        }

        // Equal in time independent of where the first difference is: a timing oracle would let a forger find
        // the signature byte by byte.
        bool ConstantTimeEqual( std::span<const std::byte> a, std::span<const std::byte> b )
        {
            std::byte difference{ 0 };
            for ( std::size_t i = 0; i < a.size(); ++i )
            {
                difference |= a[i] ^ b[i];
            }
            return difference == std::byte{ 0 };
        }
    } // namespace

    std::vector<std::byte> IssueConnectToken( const ConnectToken& token, std::span<const std::byte> secret,
                                              std::string& error )
    {
        if ( secret.size() < kMinTokenSecretSize )
        {
            error = std::format( "token secret is {} bytes; at least {} are required", secret.size(),
                                 kMinTokenSecretSize );
            return {};
        }
        std::vector<std::byte> out;
        out.reserve( kConnectTokenSize );
        Put( out, kConnectTokenFormat, 1 );
        Put( out, token.Account, 8 );
        Put( out, token.Server, 8 );
        Put( out, token.ExpiresUnixSeconds, 8 );
        Mac mac;
        if ( !HmacSha256( secret, out, mac ) )
        {
            error = "HMAC-SHA256 failed in the platform crypto";
            return {};
        }
        out.insert( out.end(), mac.begin(), mac.end() );
        return out;
    }

    TokenVerdict VerifyConnectToken( std::span<const std::byte> token, std::span<const std::byte> secret,
                                     std::uint64_t serverId, std::uint64_t nowUnixSeconds )
    {
        TokenVerdict verdict;
        if ( secret.size() < kMinTokenSecretSize || token.size() != kConnectTokenSize ||
             token[0] != static_cast<std::byte>( kConnectTokenFormat ) )
        {
            return verdict;
        }
        Mac expected;
        if ( !HmacSha256( secret, token.first( kSignedSize ), expected ) ||
             !ConstantTimeEqual( expected, token.subspan( kSignedSize ) ) )
        {
            return verdict;
        }
        const ConnectToken fields{ Get( token, 1 ), Get( token, 9 ), Get( token, 17 ) };
        if ( fields.Server != serverId )
        {
            return verdict;
        }
        verdict.Token = fields;
        verdict.Check = fields.ExpiresUnixSeconds <= nowUnixSeconds ? ETokenCheck::Expired : ETokenCheck::Valid;
        return verdict;
    }

    std::uint64_t UnixSecondsNow()
    {
        return static_cast<std::uint64_t>( std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch() )
                                                .count() );
    }
} // namespace Desert::Net

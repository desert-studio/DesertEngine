#include "Net/NetHandshake.hpp"

#include "Net/NetTransport.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    namespace
    {
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

        // type, magic, protocol, cook hash, u16 token size; then the token.
        constexpr std::size_t kHelloHeaderSize = 1 + 4 + 4 + 8 + 2;
        constexpr std::size_t kWelcomeSize     = 1 + 4 + 8 + 8;
        constexpr std::size_t kRefuseSize  = 1 + 1;

        bool IsReason( std::uint8_t value )
        {
            return value >= static_cast<std::uint8_t>( ERefuseReason::NotAHello ) &&
                   value <= static_cast<std::uint8_t>( ERefuseReason::TokenInvalid );
        }
    } // namespace

    std::string_view RefuseReasonText( ERefuseReason reason )
    {
        switch ( reason )
        {
            case ERefuseReason::NotAHello:
                return "first message is not a hello";
            case ERefuseReason::ProtocolVersion:
                return "protocol version mismatch";
            case ERefuseReason::CookHash:
                return "cook hash mismatch";
            case ERefuseReason::TokenExpired:
                return "token expired";
            case ERefuseReason::TokenInvalid:
                return "token invalid";
        }
        return "unknown refusal";
    }

    int RefuseCloseCode( ERefuseReason reason )
    {
        return kAppCloseCodeMin + static_cast<int>( reason );
    }

    std::vector<std::byte> EncodeHello( const HelloMessage& hello )
    {
        std::vector<std::byte> out;
        out.reserve( kHelloHeaderSize + hello.Token.size() );
        Put( out, static_cast<std::uint8_t>( EHandshakeMessage::Hello ), 1 );
        Put( out, kHelloMagic, 4 );
        Put( out, hello.ProtocolVersion, 4 );
        Put( out, hello.CookHash, 8 );
        Put( out, hello.Token.size(), 2 );
        out.insert( out.end(), hello.Token.begin(), hello.Token.end() );
        return out;
    }

    std::vector<std::byte> EncodeWelcome( std::uint64_t cookHash, std::uint64_t pawnPrefab )
    {
        std::vector<std::byte> out;
        out.reserve( kWelcomeSize );
        Put( out, static_cast<std::uint8_t>( EHandshakeMessage::Welcome ), 1 );
        Put( out, kProtocolVersion, 4 );
        Put( out, cookHash, 8 );
        Put( out, pawnPrefab, 8 );
        return out;
    }

    std::vector<std::byte> EncodeRefuse( ERefuseReason reason )
    {
        std::vector<std::byte> out;
        Put( out, static_cast<std::uint8_t>( EHandshakeMessage::Refuse ), 1 );
        Put( out, static_cast<std::uint8_t>( reason ), 1 );
        return out;
    }

    HelloVerdict EvaluateHello( std::span<const std::byte> message, const HelloPolicy& policy )
    {
        HelloVerdict verdict;
        if ( message.size() < kHelloHeaderSize ||
             Get( message, 0, 1 ) != static_cast<std::uint8_t>( EHandshakeMessage::Hello ) ||
             Get( message, 1, 4 ) != kHelloMagic )
        {
            return verdict;
        }
        verdict.Hello.ProtocolVersion = static_cast<std::uint32_t>( Get( message, 5, 4 ) );
        // The protocol first: a client of another version may lay out even the cook hash differently.
        if ( verdict.Hello.ProtocolVersion != kProtocolVersion )
        {
            verdict.Reason = ERefuseReason::ProtocolVersion;
            return verdict;
        }
        verdict.Hello.CookHash      = Get( message, 9, 8 );
        const std::size_t tokenSize = static_cast<std::size_t>( Get( message, 17, 2 ) );
        if ( message.size() != kHelloHeaderSize + tokenSize )
        {
            return verdict;
        }
        const auto token = message.subspan( kHelloHeaderSize );
        verdict.Hello.Token.assign( token.begin(), token.end() );
        const TokenVerdict check =
             VerifyConnectToken( token, policy.TokenSecret, policy.ServerId, policy.NowUnixSeconds );
        verdict.Token = check.Token;
        if ( check.Check != ETokenCheck::Valid )
        {
            verdict.Reason =
                 check.Check == ETokenCheck::Expired ? ERefuseReason::TokenExpired : ERefuseReason::TokenInvalid;
            return verdict;
        }
        if ( verdict.Hello.CookHash != policy.CookHash )
        {
            verdict.Reason = ERefuseReason::CookHash;
            return verdict;
        }
        verdict.Accepted = true;
        return verdict;
    }

    HandshakeAnswer DecodeAnswer( std::span<const std::byte> message )
    {
        HandshakeAnswer answer;
        if ( message.empty() )
        {
            return answer;
        }
        const auto kind = static_cast<std::uint8_t>( Get( message, 0, 1 ) );
        if ( kind == static_cast<std::uint8_t>( EHandshakeMessage::Welcome ) && message.size() == kWelcomeSize &&
             Get( message, 1, 4 ) == kProtocolVersion )
        {
            answer.Kind           = HandshakeAnswer::EKind::Welcome;
            answer.ServerCookHash = Get( message, 5, 8 );
            answer.PawnPrefab     = Get( message, 13, 8 );
            if ( answer.PawnPrefab == 0 ) // a server that names no pawn has no game to join
                answer.Kind = HandshakeAnswer::EKind::Malformed;
        }
        else if ( kind == static_cast<std::uint8_t>( EHandshakeMessage::Refuse ) &&
                  message.size() == kRefuseSize && IsReason( static_cast<std::uint8_t>( Get( message, 1, 1 ) ) ) )
        {
            answer.Kind   = HandshakeAnswer::EKind::Refuse;
            answer.Reason = static_cast<ERefuseReason>( Get( message, 1, 1 ) );
        }
        return answer;
    }
} // namespace Desert::Net

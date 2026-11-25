#include "Net/NetHandshake.hpp"

#include "Net/NetTransport.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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

        // type, magic, protocol, cook hash, u16 token size; then the token, u16 join options size, the options.
        constexpr std::size_t kHelloHeaderSize = 1 + 4 + 4 + 8 + 2;
        constexpr std::size_t kWelcomeSize     = 1 + 4 + 8 + 8;
        // type, reason, u16 text size; then the text.
        constexpr std::size_t kRefuseHeaderSize = 1 + 1 + 2;

        bool IsReason( std::uint8_t value )
        {
            return value >= static_cast<std::uint8_t>( ERefuseReason::NotAHello ) &&
                   value <= static_cast<std::uint8_t>( ERefuseReason::PreLogin );
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
                return TokenCheckText( ETokenCheck::Expired );
            case ERefuseReason::TokenMalformed:
                return TokenCheckText( ETokenCheck::Malformed );
            case ERefuseReason::TokenUnknownKey:
                return TokenCheckText( ETokenCheck::UnknownKey );
            case ERefuseReason::TokenSignature:
                return TokenCheckText( ETokenCheck::Signature );
            case ERefuseReason::TokenAudience:
                return TokenCheckText( ETokenCheck::Audience );
            case ERefuseReason::TokenLifetime:
                return TokenCheckText( ETokenCheck::Lifetime );
            case ERefuseReason::TokenReplayed:
                return TokenCheckText( ETokenCheck::Replayed );
            case ERefuseReason::PreLogin:
                return "refused by the game";
        }
        return "unknown refusal";
    }

    ERefuseReason RefuseReasonOf( ETokenCheck check )
    {
        switch ( check )
        {
            case ETokenCheck::Expired:
                return ERefuseReason::TokenExpired;
            case ETokenCheck::UnknownKey:
                return ERefuseReason::TokenUnknownKey;
            case ETokenCheck::Signature:
                return ERefuseReason::TokenSignature;
            case ETokenCheck::Audience:
                return ERefuseReason::TokenAudience;
            case ETokenCheck::Lifetime:
                return ERefuseReason::TokenLifetime;
            case ETokenCheck::Replayed:
                return ERefuseReason::TokenReplayed;
            case ETokenCheck::Valid:
            case ETokenCheck::Malformed:
                break;
        }
        return ERefuseReason::TokenMalformed;
    }

    int RefuseCloseCode( ERefuseReason reason )
    {
        return kAppCloseCodeMin + static_cast<int>( reason );
    }

    std::vector<std::byte> EncodeHello( const HelloMessage& hello )
    {
        std::vector<std::byte> out;
        out.reserve( kHelloHeaderSize + hello.Token.size() + 2 + hello.JoinOptions.size() );
        Put( out, static_cast<std::uint8_t>( EHandshakeMessage::Hello ), 1 );
        Put( out, kHelloMagic, 4 );
        Put( out, hello.ProtocolVersion, 4 );
        Put( out, hello.CookHash, 8 );
        Put( out, hello.Token.size(), 2 );
        out.insert( out.end(), hello.Token.begin(), hello.Token.end() );
        Put( out, hello.JoinOptions.size(), 2 );
        for ( const char c : hello.JoinOptions )
            out.push_back( static_cast<std::byte>( c ) );
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

    std::vector<std::byte> EncodeRefuse( ERefuseReason reason, std::string_view text )
    {
        text = text.substr( 0, kMaxRefusalText );
        std::vector<std::byte> out;
        out.reserve( kRefuseHeaderSize + text.size() );
        Put( out, static_cast<std::uint8_t>( EHandshakeMessage::Refuse ), 1 );
        Put( out, static_cast<std::uint8_t>( reason ), 1 );
        Put( out, text.size(), 2 );
        for ( const char c : text )
            out.push_back( static_cast<std::byte>( c ) );
        return out;
    }

    HelloVerdict EvaluateHello( std::span<const std::byte> message, const HelloPolicy& policy,
                                ConnectTokenVerifier& tokens )
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
        if ( message.size() < kHelloHeaderSize + tokenSize + 2 )
        {
            return verdict;
        }
        const std::size_t optionsSize =
             static_cast<std::size_t>( Get( message, kHelloHeaderSize + tokenSize, 2 ) );
        if ( optionsSize > kMaxJoinOptions || message.size() != kHelloHeaderSize + tokenSize + 2 + optionsSize )
        {
            return verdict;
        }
        const auto token   = message.subspan( kHelloHeaderSize, tokenSize );
        const auto options = message.subspan( kHelloHeaderSize + tokenSize + 2 );
        verdict.Hello.Token.assign( token.begin(), token.end() );
        verdict.Hello.JoinOptions.assign( reinterpret_cast<const char*>( options.data() ), options.size() );
        TokenVerdict check = tokens.Verify( token, policy.NowUnixSeconds );
        verdict.Token      = std::move( check.Token );
        if ( check.Check != ETokenCheck::Valid )
        {
            verdict.Reason = RefuseReasonOf( check.Check );
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
                  message.size() >= kRefuseHeaderSize &&
                  message.size() == kRefuseHeaderSize + Get( message, 2, 2 ) &&
                  IsReason( static_cast<std::uint8_t>( Get( message, 1, 1 ) ) ) )
        {
            answer.Kind   = HandshakeAnswer::EKind::Refuse;
            answer.Reason = static_cast<ERefuseReason>( Get( message, 1, 1 ) );
            const auto text = message.subspan( kRefuseHeaderSize );
            answer.ReasonText.assign( reinterpret_cast<const char*>( text.data() ), text.size() );
        }
        return answer;
    }
} // namespace Desert::Net

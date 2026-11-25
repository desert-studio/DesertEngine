#include "ServerPreLogin.hpp"

#include <charconv>
#include <format>
#include <utility>

namespace Desert::Server
{
    std::optional<std::string_view> ParseOption( std::string_view options, std::string_view key )
    {
        for ( std::size_t at = options.find( '?' ); at != std::string_view::npos; )
        {
            const std::size_t next = options.find( '?', at + 1 );
            const auto pair = options.substr( at + 1, next == std::string_view::npos ? next : next - at - 1 );
            const std::size_t equal = pair.find( '=' );
            if ( pair.substr( 0, equal ) == key )
                return equal == std::string_view::npos ? std::string_view() : pair.substr( equal + 1 );
            at = next;
        }
        return std::nullopt;
    }

    ServerPreLogin::ServerPreLogin( std::set<std::uint64_t> refusedAccounts )
         : m_RefusedAccounts( std::move( refusedAccounts ) )
    {
    }

    Net::PreLoginResult ServerPreLogin::PreLogin( const Net::PreLoginRequest& request )
    {
        const std::string_view                claims( reinterpret_cast<const char*>( request.Claims.data() ),
                                                      request.Claims.size() );
        const std::optional<std::string_view> account = ParseOption( claims, "account" );
        std::uint64_t                         id      = 0;
        const bool                            parsed  = account && !account->empty() &&
                            std::from_chars( account->data(), account->data() + account->size(), id ).ptr ==
                                 account->data() + account->size() &&
                            id != 0;
        if ( !parsed )
            return { std::format( "the token's claims name no account ('{}')", claims ), 0 };
        if ( m_RefusedAccounts.contains( id ) )
            return { std::format( "account {} is not admitted to this world", id ), 0 };
        return { {}, id };
    }
} // namespace Desert::Server

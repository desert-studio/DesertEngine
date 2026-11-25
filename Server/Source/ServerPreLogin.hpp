#pragma once

// DESERTSERVER'S GAME LAYER - its PreLogin (UE AGameModeBase::PreLogin; Net/PreLogin.hpp). The claims are THIS
// game's format, parsed here and nowhere in Net: a UE options string the issuer signed ("?account=42"). The rules
// come from the server's config (its command line): --refuse-account <id> refuses that account with the game's
// own reason, which Net sends to the client and logs.

#include <Net/PreLogin.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string_view>

namespace Desert::Server
{
    // UE UGameplayStatics::ParseOption: the value of "?key=value" in @p options ("" for a bare "?key"), or
    // nullopt.
    std::optional<std::string_view> ParseOption( std::string_view options, std::string_view key );

    class ServerPreLogin final : public Net::IPreLogin
    {
    public:
        explicit ServerPreLogin( std::set<std::uint64_t> refusedAccounts );

        // Admits the account its claims name ("?account=<id>", id > 0) unless the config refuses it.
        Net::PreLoginResult PreLogin( const Net::PreLoginRequest& request ) override;

    private:
        std::set<std::uint64_t> m_RefusedAccounts;
    };
} // namespace Desert::Server

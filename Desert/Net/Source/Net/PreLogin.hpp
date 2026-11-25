#pragma once

// THE GAME'S ADMISSION RULE (UE AGameModeBase::PreLogin): Net admits a connection whose Hello carries the right
// protocol, a connect token signed by a trusted issuer for this world (ConnectToken.hpp) and the same cook; then
// it asks the game. The game reads the token's CLAIMS - opaque to Net, in whatever format its issuer writes - and
// the client's join options, and admits or refuses with its own reason string, which the client receives and the
// server logs (UE: PreLogin's ErrorMessage). Net holds no account, ban or capacity rule: those are the game's.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Net
{
    struct PreLoginRequest
    {
        std::span<const std::byte> Claims;     // as the issuer signed them
        std::uint64_t              Issuer = 0; // which issuer signed them
        std::string_view JoinOptions; // the client's own options from its Hello (UE: the URL's options); unsigned
        std::string_view Address;     // where the connection comes from
    };

    struct PreLoginResult
    {
        std::string   Refusal;     // empty = admitted; else the game's reason (UE PreLogin's ErrorMessage)
        std::uint64_t Account = 0; // who the game admits (UE: the UniqueNetId), handed to the sim's PlayerJoined
    };

    // Implemented by the server's game layer (UE: its GameMode) and handed to every gateway it runs.
    class IPreLogin
    {
    public:
        virtual ~IPreLogin()                                              = default;
        virtual PreLoginResult PreLogin( const PreLoginRequest& request ) = 0;
    };

    // The longest refusal text carried to the client.
    inline constexpr std::size_t kMaxRefusalText = 512;
} // namespace Desert::Net

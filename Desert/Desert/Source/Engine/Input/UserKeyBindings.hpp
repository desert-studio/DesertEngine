#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>

#include <filesystem>
#include <string>
#include <vector>

// THE PLAYER'S OWN KEYS (GP1b) — UE's UEnhancedInputUserSettings / player-mappable keys: a key the player chose
// for one mapping of one context, saved to the USER's config and never to the asset. The asset keeps the
// designer's key; the override names that key (DefaultKey) so a mapping is found again after the asset is
// re-saved, and an override whose mapping is gone simply matches nothing.
namespace Desert::Input
{
    struct UserKeyOverride
    {
        std::string Context;    ///< the mapping context's header GUID (AssetGuidToText)
        std::string Action;     ///< the mapped action's header GUID (AssetGuidToText)
        std::string DefaultKey; ///< the key the context's asset maps (InputKeyFromName's spelling)
        std::string Key;        ///< the key the player chose instead

        [[nodiscard]] bool operator==( const UserKeyOverride& ) const = default;
    };

    struct UserKeyBindings
    {
        std::vector<UserKeyOverride> Overrides;

        [[nodiscard]] bool operator==( const UserKeyBindings& ) const = default;
    };
    DESERT_JSON_STRUCT( UserKeyBindings, "UserKeyBindings", 1 )

    /// Where this user's bindings live: `input.json` beside the host's machine.json (the editor's
    /// ~/.desertengine, a game's per-user product directory — Common::Settings::GameUserDirectory). Empty when
    /// the host never loaded its machine settings, i.e. has no user directory to save into.
    std::filesystem::path UserKeyBindingsFile();

    /// Reads @p file. A missing file is no override; a key name InputKeyFromName does not know is an error
    /// naming it (a binding that would silently do nothing is refused, not dropped).
    Common::ResultStr<UserKeyBindings> LoadUserKeyBindings( const std::filesystem::path& file );

    /// Writes @p bindings to @p file atomically.
    Common::BoolResultStr SaveUserKeyBindings( const std::filesystem::path& file,
                                               const UserKeyBindings&       bindings );
} // namespace Desert::Input

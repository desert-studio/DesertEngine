#pragma once

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <vector>

namespace Desert::Scripting
{
    // A single editor-exposed script variable. Declared by a script via its global `Properties` table; the
    // editor reads the schema (names/types/defaults), the user can override per entity, and the values are
    // written back into the script's environment before it runs. POD only (NO sol/Lua types) so this can
    // live in ScriptComponent / Components.hpp without leaking the scripting backend.
    enum class PropertyType
    {
        Number = 0,
        Bool   = 1,
        String = 2
    };

    struct ScriptProperty
    {
        std::string  Name;
        PropertyType Type   = PropertyType::Number;
        double       Number = 0.0;
        bool         Bool   = false;
        std::string  Str;
    };

    // The names a script marks SaveGame - UE's SaveGame flag on a Blueprint variable. Declared in the script
    // beside its `Properties` table, by name:
    //
    //     Properties         = { Score = 0, Coins = 0, WalkSpeed = 300 }
    //     SaveGameProperties = { "Score", "Coins" }
    //
    // A SaveGame property is game state, not tuning: what the running script writes to `Properties.<name>` is
    // read back into its slot (ScriptEngine::ReadBackProperties) and a save game stores it per entity UUID +
    // script + property name (Core/SaveGame.hpp). Read from the FILE, never stored in the scene: the script is
    // the one declaration. A script without the list marks none. Refused by name: an unreadable script, a
    // `SaveGameProperties` that is not a list of strings, a name its `Properties` does not declare.
    [[nodiscard]] Common::ResultStr<std::vector<std::string>>
    ReadScriptSaveGameProperties( const std::string& path );
} // namespace Desert::Scripting

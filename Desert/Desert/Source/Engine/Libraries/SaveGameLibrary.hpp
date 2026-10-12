#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Libraries
{
    /// Save games for gameplay code — UGameplayStatics::SaveGameToSlot, LoadGameFromSlot, DoesSaveGameExist,
    /// DeleteGameInSlot and the platform save list, over Core/SaveGame.hpp (the same calls C++ gameplay makes).
    /// `user` is the UserIndex (0 by default). A failure answers as a value of nil/false plus the reason, never as a
    /// script error.
    struct SaveGameLibrary
    {
        REFLECT( ScriptName( "savegame" ) )

        FUNCTION( ScriptCallable, ScriptName( "save" ),
                  Tooltip( "Saves the played world's pawn and SaveGame properties into the slot." ) )
        static Common::BoolResultStr Save( const std::string& slot, std::uint32_t user = 0 );

        FUNCTION( ScriptCallable, ScriptName( "load" ),
                  Tooltip( "Applies the slot onto the played world: what it skipped (a missing entity, a field whose "
                           "type changed, each naming the entity and field), or nil and the reason." ) )
        static Common::ResultStr<std::vector<std::string>> Load( const std::string& slot, std::uint32_t user = 0 );

        FUNCTION( ScriptCallable, ScriptName( "exists" ) )
        static bool Exists( const std::string& slot, std::uint32_t user = 0 );

        FUNCTION( ScriptCallable, ScriptName( "delete" ) )
        static Common::BoolResultStr Delete( const std::string& slot, std::uint32_t user = 0 );

        FUNCTION( ScriptCallable, ScriptName( "list" ), Tooltip( "The user's slot names, or nil and the reason." ) )
        static Common::ResultStr<std::vector<std::string>> List( std::uint32_t user = 0 );

        FUNCTION( ScriptCallable, ScriptName( "scene" ),
                  Tooltip( "The scene the slot was saved in ({guid=, name=}), so a script can level.open it before "
                           "savegame.load; nil and the reason." ) )
        static Common::ResultStr<Reflection::Value::Map> Scene( const std::string& slot, std::uint32_t user = 0 );
    };
} // namespace Desert::Libraries

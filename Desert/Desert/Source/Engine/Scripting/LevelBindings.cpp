#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/LevelTravel.hpp>

#include <tuple>

namespace Desert::Scripting
{
    // level.open( name ) -- Core::OpenLevel, the same call a UI button and C++ gameplay make: it QUEUES the
    // travel and the host applies it at the next frame boundary, so the script that called it finishes its
    // callback in the level it is running in. `name` is project-relative (Content/Scenes/Arena.desce) or
    // absolute; level.open() with no name opens the project's default map. Returns true, or
    // false + the reason (with the path that was tried) when the level does not resolve.
    void RegisterLevelBindings( ScriptEngine::Impl& implRef )
    {
        sol::table level = implRef.Lua.create_named_table( "level" );
        level.set_function( "open",
                            []( sol::optional<std::string> name ) -> std::tuple<bool, sol::optional<std::string>>
                            {
                                const auto opened = Core::OpenLevel( name.value_or( std::string{} ) );
                                if ( !opened )
                                    return { false, opened.GetError() };
                                return { true, sol::nullopt };
                            } );
    }
} // namespace Desert::Scripting

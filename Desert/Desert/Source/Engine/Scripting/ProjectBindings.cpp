#include "Internal/ScriptRuntime.hpp"

#include <Engine/Project/GameSettings.hpp>
#include <Engine/Project/ProjectContext.hpp>

namespace Desert::Scripting
{
    // project.name() / project.company() — what a title screen and the credits print. Read from the project's
    // own settings (the .deproj's Name, Config/Game.json's Company), so renaming the game is one edit there and
    // no script or string table carries the name.
    void RegisterProjectBindings( ScriptEngine::Impl& implRef )
    {
        auto&      lua     = implRef.Lua;
        sol::table project = lua.create_named_table( "project" );
        project.set_function( "name",
                              []
                              {
                                  return Project::ProjectContext::HasProject() ? Project::ProjectContext::Current().Name
                                                                               : std::string{};
                              } );
        project.set_function( "company", [] { return Project::CurrentGameSettings().Company; } );
    }
} // namespace Desert::Scripting

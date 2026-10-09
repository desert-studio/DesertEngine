#include "Internal/ScriptRuntime.hpp"

#include <Engine/Project/GameSettings.hpp>
#include <Engine/Project/ProjectContext.hpp>

namespace Desert::Scripting
{
    namespace
    {
        int Name( lua_State* L )
        {
            lua_pushstring( L, Project::ProjectContext::HasProject() ? Project::ProjectContext::Current().Name.c_str() : "" );
            return 1;
        }
        int Company( lua_State* L )
        {
            const std::string company = Project::CurrentGameSettings().Company;
            lua_pushstring( L, company.c_str() );
            return 1;
        }
    } // namespace

    // project.name() / project.company() — the open project's identity.
    void RegisterProjectBindings( lua_State* L )
    {
        constexpr luaL_Reg kProject[] = { { "name", &Name }, { "company", &Company }, { nullptr, nullptr } };
        luaL_register( L, "project", kProject );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting

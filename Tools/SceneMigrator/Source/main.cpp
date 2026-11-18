// Thin entry point. Everything the tool does — collect, parse, migrate, report, write back, exit
// code — is RunSceneMigrator in MigratorMain.cpp, behind a callable signature so the write path is
// compiled and driven by Tests/Tools/SceneMigratorWritePath the way the pure migrations already are.
// The one thing main() owns is turning argv into strings and the return into the process exit code.

#include <ToolEngineDir.hpp>
#include <ToolMain.hpp>

#include <Common/Core/CrashHandler.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include "MigratorMain.hpp"

#include <iostream>
#include <string>
#include <vector>

static int RunTool( int argc, char** argv )
{
    // THE HANDLER IS INSTALLED HERE AND NOT IN ToolMain.hpp. That header is deliberately
    // dependency-free — six of the thirteen tools link nothing but a vendored stb — so putting a
    // Common dependency in it would break the tools that cannot have one. This tool links Common, so
    // it installs its own, once, with no project: a migrator is pointed at paths, not opened on a
    // project, so its reports belong in the per-user crash folder.
    Common::Crash::InstallOptions crashOptions;
    crashOptions.hostName = "SceneMigrator";
    if ( const Common::BoolResultStr installed = Common::Crash::Install( crashOptions ); !installed.IsSuccess() )
    {
        std::cerr << "SceneMigrator: crash handler: " << installed.GetError() << '\n';
        return 1;
    }

    // Engine shaders resolve off the engine directory beside the tool (ENG-ROOT): a material's shader locator
    // follows its GUID to where that engine shader lives now (FollowShaderLocator), never to the cwd.
    if ( const std::string refused = Desert::Tools::SetEngineDirFromExecutable( {} ); !refused.empty() )
    {
        std::cerr << "SceneMigrator: no engine directory: " << refused << '\n';
        return 1;
    }

    const std::vector<std::string> args( argv + 1, argv + argc );
    // The reflected types register from the static object in the generated Reflection.gen.cpp. Since BUILD-FAST-1
    // this tool LINKS the engine libraries instead of compiling that file, and a static library's object is linked
    // only when something references it (the engine's one reference is Renderer.cpp, which no step reaches) - so
    // without this call the registry is empty and the canonical pass refuses every scene ("the reflected type
    // 'SceneSettings' is not registered in this build"). Same reference as Tests/TestSupport/EngineReflectionLink.cpp.
    Desert::Reflection::ForceLinkGeneratedReflection();
    return Desert::Migration::RunSceneMigrator( args, std::cout, std::cerr );
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "SceneMigrator", argc, argv, &RunTool );
}

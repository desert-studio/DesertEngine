// Thin entry point. Everything the tool does — collect, parse, migrate, report, write back, exit
// code — is RunSceneMigrator in MigratorMain.cpp, behind a callable signature so the write path is
// compiled and driven by Tests/Tools/SceneMigratorWritePath the way the pure migrations already are.
// The one thing main() owns is turning argv into strings and the return into the process exit code.

#include <ToolMain.hpp>

#include <Common/Core/CrashHandler.hpp>

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

    const std::vector<std::string> args( argv + 1, argv + argc );
    return Desert::Migration::RunSceneMigrator( args, std::cout, std::cerr );
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "SceneMigrator", argc, argv, &RunTool );
}

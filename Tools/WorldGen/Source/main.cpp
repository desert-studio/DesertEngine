// Thin entry point. Everything the tool does lives in RunWorldGen so the suite can drive the same path.

#include <ToolMain.hpp>
#include <ToolModules.hpp>
#include <LandscapeCore/Public/LandscapeWorldPartition.hpp>
#include <ToolEngineDir.hpp>

#include "WorldGenMain.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static int RunTool( int argc, char** argv )
{
    // `--engine-dir <dir>` belongs to the process, not to the generator: it is taken out here, and the
    // engine directory is resolved from this executable when it is absent (ToolEngineDir.hpp). It is
    // never a default --project: RunWorldGen refuses a run that needs a project and names none. A tool given both
    // --project and --assets needs none, so only then is a missing one not a refusal. `--disable-module <Name>`
    // is the process's too: the landscape world-partition descriptor hooks (BUILD-MOD-6L) are started unless it
    // names LandscapeCore (ToolModules.hpp).
    const Desert::Tools::StartedToolModules started =
        Desert::Tools::StartToolModules( argc, argv, { { "LandscapeCore", &Desert::LandscapeCore::StartupModule } } );
    if ( !started.Refused.empty() )
    {
        std::cerr << "WorldGen: " << started.Refused << "\n";
        return 2;
    }
    for ( const std::string& name : started.Disabled )
        std::cerr << "WorldGen: module " << name << " not started (--disable-module)\n";
    std::vector<std::string> args;
    std::filesystem::path    engineDirOverride;
    bool                     namesProject = false;
    bool                     namesAssets  = false;
    for ( std::size_t i = 0; i < started.Args.size(); ++i )
    {
        const std::string& arg = started.Args[i];
        if ( arg == "--engine-dir" && i + 1 < started.Args.size() )
        {
            engineDirOverride = started.Args[++i];
            continue;
        }
        namesProject = namesProject || arg == "--project";
        namesAssets  = namesAssets || arg == "--assets";
        args.push_back( arg );
    }
    if ( const std::string refused = Desert::Tools::SetEngineDirFromExecutable( engineDirOverride );
         !refused.empty() && ( !engineDirOverride.empty() || !namesProject || !namesAssets ) )
    {
        std::cerr << "WorldGen: " << refused << "\n";
        return 2;
    }
    return Desert::WorldGen::RunWorldGen( args, std::cout, std::cerr );
}

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "WorldGen", argc, argv, &RunTool );
}

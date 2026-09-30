// Thin entry point. Everything the tool does lives in RunWorldGen so the suite can drive the same path.

#include <ToolMain.hpp>
#include <ToolEngineDir.hpp>

#include "WorldGenMain.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static int RunTool( int argc, char** argv )
{
    // `--engine-dir <dir>` belongs to the process, not to the generator: it is taken out here, and the
    // engine directory (the default --project) is resolved from this executable when it is absent
    // (ToolEngineDir.hpp). A tool given both --project and --assets needs none, so only then is a
    // missing one not a refusal.
    std::vector<std::string> args;
    std::filesystem::path    engineDirOverride;
    bool                     namesProject = false;
    bool                     namesAssets  = false;
    for ( int i = 1; i < argc; ++i )
    {
        const std::string arg = argv[i];
        if ( arg == "--engine-dir" && i + 1 < argc )
        {
            engineDirOverride = argv[++i];
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

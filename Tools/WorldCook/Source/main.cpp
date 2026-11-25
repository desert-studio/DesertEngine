// Thin entry point. Everything the tool does lives in RunWorldCook so the suite can drive the same path.

#include <ToolMain.hpp>
#include <ToolModules.hpp>
#include <LandscapeCore/Public/LandscapeWorldPartition.hpp>

#include "WorldCookMain.hpp"

#include <iostream>
#include <string>

static int RunTool( int argc, char** argv )
{
    // The modules this process starts: the landscape's partition rows and hooks, so a cook places its tiles.
    // `--disable-module LandscapeCore` cooks as a build without the module would (ToolModules.hpp).
    const Desert::Tools::StartedToolModules started =
        Desert::Tools::StartToolModules( argc, argv, { { "LandscapeCore", &Desert::LandscapeCore::StartupModule } } );
    if ( !started.Refused.empty() )
    {
        std::cerr << "WorldCook: " << started.Refused << "\n";
        return 2;
    }
    for ( const std::string& name : started.Disabled )
        std::cout << "WorldCook: module " << name << " not started (--disable-module)\n";
    return Desert::WorldCook::RunWorldCook( started.Args, std::cout, std::cerr );
}

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "WorldCook", argc, argv, &RunTool );
}

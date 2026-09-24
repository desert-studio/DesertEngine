// Thin entry point. Everything the tool does lives in RunWorldCook so the suite can drive the same path.

#include <ToolMain.hpp>

#include "WorldCookMain.hpp"

#include <iostream>
#include <string>
#include <vector>

static int RunTool( int argc, char** argv )
{
    const std::vector<std::string> args( argv + 1, argv + argc );
    return Desert::WorldCook::RunWorldCook( args, std::cout, std::cerr );
}

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "WorldCook", argc, argv, &RunTool );
}

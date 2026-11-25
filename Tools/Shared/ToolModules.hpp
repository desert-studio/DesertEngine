#pragma once

// THE MODULES A TOOL PROCESS STARTS, AND `--disable-module <Name>` (UE: the module manager at PreInit starting
// the enabled plugins' modules; `-DisablePlugins=` names the ones a run skips). A tool's main lists the modules it
// starts; `--disable-module` belongs to the process, not to the tool's body, so it is taken out of the arguments
// here and the module named is not started. The run then sees what a process without that module sees: a world
// cooked or generated without LandscapeCore reports every landscape record as an Issue and places none
// (WorldPartitionPlan::UnregisteredRecords). A name no listed module has, or a flag without a name, is a refusal
// and starts nothing.

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Tools
{
    struct ToolModule
    {
        std::string_view Name;
        void ( *Startup )();
    };

    struct StartedToolModules
    {
        std::vector<std::string> Args;     // argv[1..] without the `--disable-module <Name>` pairs
        std::vector<std::string> Disabled; // the modules this run did not start, in the order named
        std::string              Refused;  // non-empty: nothing was started, the tool must not run
    };

    /// Starts every module of @p modules (in their order) but those a `--disable-module <Name>` pair of @p argv
    /// names, and returns the remaining arguments for the tool's body.
    inline StartedToolModules StartToolModules( int argc, char** argv, std::initializer_list<ToolModule> modules )
    {
        StartedToolModules started;
        for ( int i = 1; i < argc; ++i )
        {
            const std::string_view arg = argv[i];
            if ( arg != "--disable-module" )
            {
                started.Args.emplace_back( arg );
                continue;
            }
            if ( i + 1 >= argc )
            {
                started.Refused = "--disable-module names no module";
                return started;
            }
            const std::string_view name  = argv[++i];
            const auto             known = std::find_if( modules.begin(), modules.end(),
                                                         [&]( const ToolModule& module ) { return module.Name == name; } );
            if ( known == modules.end() )
            {
                started.Refused =
                     "--disable-module '" + std::string( name ) + "': this tool starts no such module (";
                for ( const ToolModule& module : modules )
                    started.Refused += ( &module == modules.begin() ? "" : ", " ) + std::string( module.Name );
                started.Refused += ")";
                return started;
            }
            if ( std::find( started.Disabled.begin(), started.Disabled.end(), name ) == started.Disabled.end() )
                started.Disabled.emplace_back( name );
        }
        for ( const ToolModule& module : modules )
            if ( std::find( started.Disabled.begin(), started.Disabled.end(), module.Name ) ==
                 started.Disabled.end() )
                module.Startup();
        return started;
    }
} // namespace Desert::Tools

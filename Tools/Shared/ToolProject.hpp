#pragma once

// A TOOL OPENS A PROJECT ONE WAY (UE: a commandlet is handed the .uproject and the engine mounts it before the
// commandlet runs). GamePackager, TextureCook and WorldCook each take a `.deproj`; the steps that turn that
// argument into an open project — the descriptor exists, the engine directory comes from the executable
// (ToolEngineDir.hpp), the log goes to <ProjectDir>/Saved/Logs, the project is opened outside the
// recent-projects registry — are this one call, so a fourth tool cannot open a project differently.
//
// Like ToolEngineDir.hpp this header is NOT dependency-free: only a tool that links the engine's Project
// module and Common may include it.

#include "ToolEngineDir.hpp"

#include <Projects/ProjectContext.hpp>

#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace Desert::Tools
{
    struct OpenedToolProject
    {
        std::filesystem::path Descriptor; // absolute path of the .deproj that was opened
        std::string           Refused;    // empty once the project is open; otherwise why it is not
    };

    // Opens `descriptor` (a .deproj path as the caller typed it, resolved against the caller's working
    // directory) for an unattended tool. `engineDirOverride` is the tool's `--engine-dir`, empty when none.
    inline OpenedToolProject OpenToolProject( const std::string&           descriptor,
                                              const std::filesystem::path& engineDirOverride )
    {
        OpenedToolProject opened;
        std::error_code   ec;
        opened.Descriptor = std::filesystem::absolute( descriptor, ec );
        if ( ec || !std::filesystem::is_regular_file( opened.Descriptor, ec ) )
        {
            opened.Refused = std::format( "'{}' is not a .deproj file this process can read", descriptor );
            return opened;
        }
        // The engine directory from the executable (or --engine-dir), never from the working directory.
        if ( std::string refused = SetEngineDirFromExecutable( engineDirOverride ); !refused.empty() )
        {
            opened.Refused = std::move( refused );
            return opened;
        }
        Common::Logger::LogInit();
        // engine_log.txt in <ProjectDir>/Saved/Logs, not in whatever folder the shell happened to be in.
        Common::Logger::RelocateLogFile( opened.Descriptor.parent_path() / "Saved" / "Logs" );
        // NOT in the recent-projects registry: a build machine opening a project is not a person opening one,
        // and every CI run would otherwise file its checkout at the top of somebody's list.
        if ( !Desert::Project::ProjectContext::Open( opened.Descriptor.string(),
                                                     Desert::Project::ProjectContext::RecordInRecent::No ) )
            opened.Refused =
                 std::format( "could not open '{}' (missing or corrupt .deproj)", opened.Descriptor.string() );
        return opened;
    }
} // namespace Desert::Tools

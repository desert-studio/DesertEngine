#pragma once

// A TOOL FINDS THE ENGINE THE WAY THE EDITOR DOES (UE: FPaths::EngineDir), never by standing somewhere.
//
// The tools that link the engine (GamePackager, TextureCook, WorldGen) used to `chdir` into the project's
// folder so that `Resources/...` and `../build/Bin/...` meant something, which made the working directory
// part of every tool's input. They now resolve the engine directory from the executable's own position —
// or from `--engine-dir`, which wins — with the one resolver the editor uses
// (Desert::Project::ResolveEngineDir), and hand it to Common::Constants::Path::SetEngineDir. The process
// does not change directory; a path the caller typed keeps meaning what it meant in the caller's shell.
//
// Unlike ToolMain.hpp this header is NOT dependency-free: only a tool that compiles
// Engine/Project/StartupLayout.cpp and links Common may include it.

#include <Engine/Project/StartupLayout.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>
#include <string>

namespace Desert::Tools
{
    // Resolves and installs the engine directory. Returns the refusal (naming every place looked at) when
    // there is none — the caller stops with it — or an empty string once EngineDir() is set.
    inline std::string SetEngineDirFromExecutable( const std::filesystem::path& engineDirOverride )
    {
        const std::filesystem::path executableIn = Common::Utils::FileSystem::ExecutablePath().parent_path();
        const Desert::Project::EngineDirLookup engine =
             Desert::Project::ResolveEngineDir( executableIn, engineDirOverride );
        if ( !engine.Explanation.empty() )
            return engine.Explanation;
        Common::Constants::Path::SetEngineDir( engine.Dir );
        return {};
    }
} // namespace Desert::Tools

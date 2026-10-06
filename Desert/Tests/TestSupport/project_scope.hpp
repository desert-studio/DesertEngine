#pragma once

// THE PROJECT STEP, TAKEN THE WAY THE EDITOR TAKES IT (UE: no .uproject, no /Game). Content exists only inside an
// open project: Path::Dir / ProjectDir stop the process and FullPath of a relative spelling throws while none is
// open. A suite is a host, so it opens one the way the editor does with --project — the .deproj is READ and its
// AssetsRoot taken from the file, never guessed — first thing in main(), right after SetSuiteEngineDir:
//
//     Desert::TestSupport::SetSuiteEngineDir();
//     Desert::TestSupport::OpenSuiteProject();
//
// A test that needs a different project (a scratch tree, a synthetic root to measure a remap) opens it with a
// ProjectScope, which restores whatever was open before on exit — never ClearProject(), which would leave every
// later test in the process without content.

#include "committed_projects.hpp"
#include "scratch_dir.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace Desert::TestSupport
{
    // A committed project (a row of kCommittedProjects, relative to the checkout) as the root pair opening it
    // sets: its folder, absolute, and the AssetsRoot its .deproj names. An unreadable .deproj stops the suite
    // naming the file — every answer after it would be measured against no content.
    inline Common::Constants::Path::ProjectRootState CommittedProjectRoot( const std::string_view deproj )
    {
        const std::filesystem::path file =
             ( RepositoryRoot() / std::filesystem::path( deproj ) ).lexically_normal();
        const auto json = Common::Utils::FileSystem::ReadFileContent( file );
        if ( json )
        {
            const auto project = Common::Project::ReadProjectFile( json.GetValue() );
            if ( project )
                return { file.parent_path(), project.GetValue().AssetsRoot };
            std::fprintf( stderr, "[TestSupport] '%s' is not a project file: %s\n", file.generic_string().c_str(),
                          project.GetError().c_str() );
        }
        else
            std::fprintf( stderr, "[TestSupport] cannot read the project '%s': %s\n",
                          file.generic_string().c_str(), json.GetError().c_str() );
        std::fflush( stderr );
        std::abort();
    }

    // The suite's host step: opens a committed project for the whole process (default: the sample project the
    // editor opens, whose Content/ is the content the suites census).
    inline void OpenSuiteProject( const std::string_view deproj = kCommittedProjects[0] )
    {
        const auto root = CommittedProjectRoot( deproj );
        Common::Constants::Path::SetProjectRoot( root.ProjectDir, root.AssetsRoot );
    }

    // Opens a project for this scope and reopens the previous one (or none) after.
    class ProjectScope
    {
    public:
        ProjectScope( const std::filesystem::path& projectDir, const std::filesystem::path& assetsRoot )
             : m_Previous( Common::Constants::Path::CurrentProjectRoot() )
        {
            Common::Constants::Path::SetProjectRoot( projectDir, assetsRoot );
        }

        // A committed project, read from its .deproj.
        explicit ProjectScope( const std::string_view deproj )
             : m_Previous( Common::Constants::Path::CurrentProjectRoot() )
        {
            const auto root = CommittedProjectRoot( deproj );
            Common::Constants::Path::SetProjectRoot( root.ProjectDir, root.AssetsRoot );
        }

        ~ProjectScope()
        {
            Common::Constants::Path::SetProjectRoot( m_Previous.ProjectDir, m_Previous.AssetsRoot );
        }

        ProjectScope( const ProjectScope& )            = delete;
        ProjectScope& operator=( const ProjectScope& ) = delete;
        ProjectScope( ProjectScope&& )                 = delete;
        ProjectScope& operator=( ProjectScope&& )      = delete;

    private:
        // A copy on purpose: SetProjectRoot rewrites the state CurrentProjectRoot() refers to.
        Common::Constants::Path::ProjectRootState m_Previous;
    };
} // namespace Desert::TestSupport

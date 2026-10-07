#pragma once

// THE REPOSITORY A TEST READS COMMITTED FILES FROM, FOUND FROM THE RUNNER'S OWN PATH.
//
// A census that reads the tree (every committed scene, every shader) needs the checkout it was built
// from. Before BUILD1 two suites baked it in as a define (DESERT_SCENES_DIR); a layer runner holds many
// suites in one project, so a per-suite define has nowhere to live. The working directory is not the
// answer either: the runners start a suite in build/TestScratch/<config>/<suite>, an IDE or a shell
// anywhere. The executable is always build/Bin/Tests/<config>/<Runner> inside the checkout, so the root
// is the first ancestor of it that holds the marker file -- whatever the working directory is.

#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>

namespace Desert::TestSupport
{
    // The checkout's root, or an empty path after an ADD_FAILURE naming where the search started: a
    // census that silently walked an empty directory would pass over nothing.
    inline std::filesystem::path RepoRoot()
    {
        const std::filesystem::path executable = Common::Utils::FileSystem::ExecutablePath();
        for ( std::filesystem::path at = executable.parent_path(); !at.empty(); at = at.parent_path() )
        {
            if ( std::filesystem::exists( at / "Desert/Common/Source/Common/Core/Constants.hpp" ) )
            {
                return at;
            }
            if ( at == at.parent_path() )
            {
                break;
            }
        }
        ADD_FAILURE() << "no repository root (Desert/Common/Source/Common/Core/Constants.hpp) above the runner "
                      << executable;
        return {};
    }
} // namespace Desert::TestSupport

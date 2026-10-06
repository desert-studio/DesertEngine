#pragma once

// THE REPOSITORY'S COMMITTED PROJECTS — the one list every census of "the committed content" walks. ENG-ROOT
// moved content the suites own out of the editor's Sandbox into a project of their own (Desert/Tests/Data), so a
// census that opened the Sandbox alone silently lost every kind, scene and mesh reference that moved. The engine
// content every project shares (the engine directory's Resources/) is reached from whichever project is open, as
// the engine itself reaches it; the projects are listed here, each by its .deproj relative to the repository
// root. A new committed project is one row here, not one more walk in some suite.

#include <array>
#include <string_view>

namespace Desert::TestSupport
{
    inline constexpr std::array<std::string_view, 2> kCommittedProjects = {
         "Editor/Desert.deproj",                 // the Sandbox the editor opens
         "Desert/Tests/Data/DesertTests.deproj", // the suites' own project
    };
} // namespace Desert::TestSupport

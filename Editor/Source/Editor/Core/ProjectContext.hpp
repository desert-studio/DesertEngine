#pragma once

// The project system moved into the ENGINE (Engine/Project/ProjectContext) so the Runtime player can
// share it. This header keeps the historical Desert::Editor::ProjectContext spelling alive.
#include <Projects/ProjectContext.hpp>

namespace Desert::Editor
{
    using ProjectFile    = ::Desert::Project::ProjectFile;
    using ProjectContext = ::Desert::Project::ProjectContext;
} // namespace Desert::Editor

#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <vector>

namespace Desert::Editor
{
    // The Build group of the command palette: Package Game and the project's ContentChunks.json (create, reload).
    void AppendBuildCommands( std::vector<PaletteCommand>& commands );
} // namespace Desert::Editor

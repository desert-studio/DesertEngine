#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <vector>

namespace Desert::Editor
{
    // The Language group of the command palette: one entry per language the compiled locale table knows.
    void AppendLanguageCommands( std::vector<PaletteCommand>& commands );
} // namespace Desert::Editor

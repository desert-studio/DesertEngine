#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <vector>

namespace Desert::Editor
{
    // The Clouds group of the command palette: one entry per stage of the sky, each opening the Clouds window
    // ON that stage (CloudsPanel::OpenAt, the Details panel's own door).
    void AppendCloudCommands( std::vector<PaletteCommand>& commands );
} // namespace Desert::Editor

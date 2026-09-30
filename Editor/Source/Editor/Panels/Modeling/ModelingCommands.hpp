#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // The Modeling and CubeGrid groups of the command palette: every tool, button, checkbox and closed choice
    // ModelingPanel draws (suite ModelingPaletteCensus holds this list to the panel's widgets).
    // `scene` is the editor's main-scene slot, read when an entry RUNS.
    void AppendModelingCommands( std::vector<PaletteCommand>&                  commands,
                                 const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

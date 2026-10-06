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
    // The Landscape group of the command palette: every setting and button of the Landscape panel (sculpt, manage,
    // paint, edit layers, heightmap import/export), so a landscape can be made, painted and photographed
    // unattended. `scene` is the editor's main-scene slot, read when an entry RUNS (the slot may be re-pointed
    // after the build).
    void AppendLandscapeCommands( std::vector<PaletteCommand>&                  commands,
                                  const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <memory>

namespace Desert::Core
{
    class Scene;
}

#include <vector>

namespace Desert::Editor
{
    // The 2D UI entry of the View group: switch the main scene's viewport into / out of 2D UI mode. `scene` is
    // the editor's main-scene slot, read when the entry RUNS.
    void AppendUICommands( std::vector<PaletteCommand>&                  commands,
                           const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

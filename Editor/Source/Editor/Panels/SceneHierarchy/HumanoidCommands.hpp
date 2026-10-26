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
    // The procedural humanoid: spawning one into the open scene, and writing its generated engine assets
    // (a generator, not a demo scene). `scene` is the editor's main-scene slot, read when an entry RUNS.
    void AppendHumanoidCommands( std::vector<PaletteCommand>&                  commands,
                                 const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

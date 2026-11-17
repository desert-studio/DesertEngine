#pragma once

// THE DATA LAYER OUTLINER'S VERBS ON THE COMMAND PALETTE (WP-DATA-LAYER-OUTLINER), one set per layer of the active
// world - the same functions the outliner's rows call (DataLayerEditor, UE UDataLayerEditorSubsystem), so a
// scenario or a keyboard drives exactly what a click does:
//   Scene > "Data Layers: Make Current <layer>" / "Data Layers: Remove <layer> from Current"
//   Scene > "Data Layers: Add Selected to <layer>" / "Data Layers: Remove Selected from <layer>"
//   Scene > "Data Layers: Select Actors in <layer>"
//   Scene > "Data Layers: Hide <layer>" / "Data Layers: Show <layer>"

#include <Editor/Core/CommandPalette.hpp>

#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    void AppendDataLayerCommands( std::vector<PaletteCommand>&                  commands,
                                  const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

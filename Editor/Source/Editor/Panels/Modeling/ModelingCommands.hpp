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
    //
    // FOUR CALLS, because the group sits in four places of the palette's order (the entity actions, the view
    // modes, after Landscape, and the tools proper); the editor registers each where the palette shows it.
    // UE's Mesh To Collision on the selection — among the Entity actions.
    void AppendMeshToCollisionCommands( std::vector<PaletteCommand>&                  commands,
                                        const std::shared_ptr<::Desert::Core::Scene>& scene );
    // The Select Elements tool — after the viewport's authoring modes.
    void AppendSelectElementsCommand( std::vector<PaletteCommand>& commands );
    // The Create Shape tool, one entry per shape, and its placement — after Landscape.
    void AppendCreateShapeCommands( std::vector<PaletteCommand>& commands );
    // Everything else: selection modes, the tools, their options, CubeGrid.
    void AppendModelingCommands( std::vector<PaletteCommand>&                  commands,
                                 const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

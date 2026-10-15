#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    class FractureTool;

    // Generate (UE's Fracture button) from the selected entity's static mesh: the mesh is its asset, lifted the
    // way every Modeling tool reads it, and the fracture records that asset's GUID as its source. Refused by
    // name when nothing is selected, the entity has no static mesh, it carries an unaccepted modeling edit (the
    // fracture must name the asset it cut), or the asset states no GUID.
    Common::BoolResultStr GenerateFromSelection( FractureTool&                                 tool,
                                                 const std::shared_ptr<::Desert::Core::Scene>& scene );

    // The Fracture group of the command palette: every control of the Fracture panel (mode, target file, seed,
    // levels and their method / counts / planes, auto-cluster, Generate, interior material, Explode Amount and
    // Fracture Level), so the mode can be driven and photographed through the editor control socket. `scene` is
    // the editor's main-scene slot, read when an entry RUNS.
    void AppendFractureCommands( std::vector<PaletteCommand>&                  commands,
                                 const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

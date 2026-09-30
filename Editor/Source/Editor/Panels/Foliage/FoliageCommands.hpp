#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    // The Foliage group of the command palette: the mode, the palette (collections, meshes, prefabs, types), the
    // brush and the edited type, so foliage can be painted and photographed unattended.
    // `scene` and `assets` are the editor's slots, read when an entry RUNS.
    void AppendFoliageCommands( std::vector<PaletteCommand>&                  commands,
                                const std::shared_ptr<::Desert::Core::Scene>& scene,
                                const std::shared_ptr<Assets::AssetManager>&  assets );
} // namespace Desert::Editor

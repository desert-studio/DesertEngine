#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <filesystem>
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
    // `scene` and `assets` are the editor's slots, read when an entry RUNS; `assetFiles` is the palette build's
    // one census of the content root (AssetFileCensus).
    void AppendFoliageCommands( std::vector<PaletteCommand>&                  commands,
                                const std::shared_ptr<::Desert::Core::Scene>& scene,
                                const std::shared_ptr<Assets::AssetManager>&  assets,
                                const std::vector<std::filesystem::path>&     assetFiles );
} // namespace Desert::Editor

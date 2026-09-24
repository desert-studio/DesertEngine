#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Graphic/Image.hpp>

#include <memory>
#include <string>

namespace Desert::Editor::TextureDnD
{
    // Drag-and-drop / import glue for textures. The File Explorer emits SOURCE paths
    // (Resources/Textures/foo.png) but textures are registered from their `.detex` assets, so a path
    // match misses — these helpers bridge source paths to registered runtime-texture handles.

    // Resolve a dropped source path to an ALREADY-registered texture handle (exact path, then filename
    // stem). Does NOT import. Returns a zero handle when nothing matches (used for thumbnails, so browsing
    // never triggers a cook).
    Assets::AssetHandle ResolveExisting( const Assets::AssetManager& mgr, const std::string& sourcePath );

    // Resolve, or IMPORT-on-demand if not yet registered: import the source into its `.detex` asset,
    // create+register the TextureAsset, and return its handle. Returns a zero handle on failure. Used by
    // the drag-drop assignment target and the Import button.
    Assets::AssetHandle ResolveOrImport( Assets::AssetManager& mgr, const std::string& sourcePath );

    // Non-owning runtime image for a texture handle (thumbnail preview). Null if the handle isn't a
    // registered texture.
    std::shared_ptr<Graphic::Image2D> ResolveImage( const Assets::AssetHandle& handle );

} // namespace Desert::Editor::TextureDnD

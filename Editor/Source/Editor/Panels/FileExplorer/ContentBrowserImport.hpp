#pragma once

#include <filesystem>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    struct DirectoryInformation;
}

/// FILES FROM OUTSIDE THE PROJECT INTO THE OPEN FOLDER (UE: DragDropHandler.cpp + the toolbar's Import): an OS
/// file drop on the editor window and the Import button both copy into @p folder (Resources/Textures when there
/// is none) and cook + register a texture at once, so it is draggable straight away; other files appear in the
/// browser and cook on the next launch / Rebuild Cooked. Each returns whether a file landed (the folder needs a
/// re-listing).
namespace Desert::Editor::ContentBrowserImport
{
    // One external file. Nothing for no asset manager, an empty path, a missing file or a directory; a failed
    // copy is logged with both paths.
    bool ImportFile( Assets::AssetManager* assetManager, const std::filesystem::path& source,
                     const DirectoryInformation* folder );
    // The image picker, then ImportFile on the choice.
    bool ImportTextureFromDialog( Assets::AssetManager* assetManager, const DirectoryInformation* folder );
} // namespace Desert::Editor::ContentBrowserImport

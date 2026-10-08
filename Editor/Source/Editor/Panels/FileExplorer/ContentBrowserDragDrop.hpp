#pragma once

#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Panels/FileExplorer/FileType.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// A CONTENT BROWSER DRAG ONTO A FOLDER (UE: SPathView::OnAssetsOrPathsDragDropped / SAssetView's folder tiles
// -> ContentBrowserUtils::MoveAssets). Device-free: the payload type a tile is dragged as, the set a folder
// accepts, and which paths a drop moves - so the source and every folder target read one table, and a suite
// can hold the drop to a non-empty destination.
namespace Desert::Editor::ContentBrowserDragDrop
{
    /// The payload a browser entry is dragged as. Typed, so a texture slot accepts a texture and nothing else;
    /// a folder and every kind without a typed slot are the generic AssetFile.
    [[nodiscard]] inline const char* PayloadTypeOf( bool isFile, FileType type )
    {
        if ( !isFile )
            return DragPayloads::AssetFile;
        switch ( type )
        {
            case FileType::Prefab:
                return DragPayloads::PrefabFile;
            case FileType::Texture:
                return DragPayloads::TextureAsset;
            case FileType::Material:
                return DragPayloads::MaterialAsset;
            case FileType::Model:
                return DragPayloads::MeshAsset;
            case FileType::Font:
                return DragPayloads::FontFile;
            case FileType::Scene:
                return DragPayloads::SceneFile;
            default:
                return DragPayloads::AssetFile;
        }
    }

    /// Every payload PayloadTypeOf can return: a folder target accepts all of them, since whatever kind was
    /// dragged, dropping it on a folder means "move it there".
    inline constexpr std::array<const char*, 7> MovablePayloads = {
         DragPayloads::AssetFile,     DragPayloads::PrefabFile, DragPayloads::TextureAsset,
         DragPayloads::MaterialAsset, DragPayloads::MeshAsset,  DragPayloads::FontFile,
         DragPayloads::SceneFile };

    /// The paths a drop of @p dragged onto @p targetFolder moves. UE drags the SELECTION when the grabbed tile
    /// is part of it, otherwise the grabbed tile alone. Nothing moves onto an empty target, onto itself, into
    /// the folder it already lives in, or (a folder) into its own subtree.
    [[nodiscard]] inline std::vector<std::string> PlanFolderDrop( const std::string&              dragged,
                                                                  const std::vector<std::string>& selection,
                                                                  const std::string&              targetFolder )
    {
        std::vector<std::string> moves;
        if ( targetFolder.empty() || dragged.empty() )
            return moves;

        const bool draggedSelection = std::find( selection.begin(), selection.end(), dragged ) != selection.end();
        const std::vector<std::string> candidates =
             draggedSelection ? selection : std::vector<std::string>{ dragged };

        const std::filesystem::path target = std::filesystem::path( targetFolder ).lexically_normal();
        for ( const std::string& path : candidates )
        {
            const std::filesystem::path src = std::filesystem::path( path ).lexically_normal();
            if ( src.empty() || src == target || src.parent_path() == target )
                continue;
            const std::filesystem::path rel = target.lexically_relative( src );
            if ( !rel.empty() && *rel.begin() != ".." )
                continue; // the target is inside the dragged folder
            moves.push_back( path );
        }
        return moves;
    }
} // namespace Desert::Editor::ContentBrowserDragDrop

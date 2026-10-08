#pragma once

#include <ImGui/imgui.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    namespace UI
    {
        class UIHelper;
    }
    class AssetThumbnailPool;
    struct DirectoryInformation;

    /// THE PICTURE ON A TILE, ON ITS TOOLTIP AND UNDER THE CURSOR OF A DRAG (F5; UE SAssetThumbnail over
    /// FAssetThumbnailPool). Draws what AssetThumbnailPool holds and, when the picture is missing or stale,
    /// asks ThumbnailService for it — by ThumbnailProducers::ProducerOf, the one dispatch. The panel owns
    /// the pool and this drawer; the drawer never outlives either.
    class AssetTileThumbnail
    {
    public:
        AssetTileThumbnail( AssetThumbnailPool& pool, Assets::AssetManager* assetManager );
        ~AssetTileThumbnail();
        AssetTileThumbnail( const AssetTileThumbnail& )            = delete;
        AssetTileThumbnail& operator=( const AssetTileThumbnail& ) = delete;

        // The tile's and the tooltip's picture. False = draw the type icon (no picture yet, or none by design).
        bool DrawThumbnail( DirectoryInformation* entry, const ImVec2& size );

        // UE-style hover tooltip for a tile: picture, name, type/size, path. Shown after the cursor has
        // rested AssetTooltipLayout::kHoverDelaySeconds on the same tile; size and placement from
        // AssetTooltipLayout::Compute (capped, never off-window). A click only selects.
        void DrawTooltip( DirectoryInformation* entry );
        // The cursor left @p entry (or a drag began): its hover delay starts over next time.
        void ForgetTooltip( const DirectoryInformation* entry );

        // A dragged asset's preview: its picture when resident (texture/material/cloud/model), else the big
        // coloured type icon, with the filename beside it.
        void DrawDragPreview( const DirectoryInformation& entry );

        // A resident picture as an ImGui texture (ThumbnailEditMode's orbit preview); null when not resident.
        ImTextureID TextureOf( const std::string& png );

    private:
        bool DrawTextureThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // Material-on-sphere: the PNG drawn first; a missing or stale one is asked of ThumbnailService, the
        // albedo swatch standing in meanwhile.
        bool DrawRenderedMaterialThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // The mesh auto-framed by its bounds, keyed on its cooked form (AssetThumbnailPool::MeshPictureFor).
        bool DrawRenderedMeshThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // A skinned mesh in its bind pose (ThumbnailPose). @p subject is the posed asset: the entry itself, or
        // the .skmesh/.skeleton a skinned source's import wrote.
        bool DrawRenderedPoseThumbnail( DirectoryInformation* entry, const ImVec2& size,
                                        const std::string& subject );
        // The four cloud formats: PAINTED from the file's own bytes (Editor/Widgets/CloudThumbnail.hpp).
        bool DrawPaintedThumbnail( DirectoryInformation* entry, const ImVec2& size );

        AssetThumbnailPool&           m_Pool;
        Assets::AssetManager*         m_AssetManager = nullptr;
        std::unique_ptr<UI::UIHelper> m_UIHelper;
        // A capture, once asked, is the service's to finish — asked again only after the picture has been
        // seen current, so an edit that makes it stale asks again. Asset path -> its placeholder swatch.
        std::unordered_map<std::string, ImVec4> m_CaptureAsked;

        // Which tile the cursor rests on and since when; identity only, never dereferenced here.
        const DirectoryInformation* m_TooltipEntry      = nullptr;
        double                      m_TooltipHoverStart = 0.0;
    };
} // namespace Desert::Editor

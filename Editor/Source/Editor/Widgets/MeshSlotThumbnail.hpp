#pragma once

#include <Engine/Assets/Common.hpp>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    // THE LEFT HALF OF A MESH ASSET ROW (UE SObjectPropertyEntryBox: the thumbnail, then the picker beside it in
    // the same row, the row as tall as the thumbnail). One drawer for every mesh slot in Details - the reflected
    // StaticMeshComponent::StaticMesh / ImpostorMesh rows (PropertyEditorBuilder) and a primitive's Shape row - so
    // the request order and the three tooltip states below are written once.
    inline constexpr float kMeshSlotThumbnailSize = 64.0f;

    // Draws a size x size framed thumbnail at the cursor and leaves the cursor on the SAME line for the slot's
    // picker: the caller must draw an item after it, or the row's line ends at the thumbnail's top and the next
    // row is drawn over it (DETAILS-SM-PREVIEW). The picture is the cached PNG of @p mesh, asked through
    // ThumbnailService; with no mesh, the cached picture of @p fallbackMaterial (a primitive's slot 0); a framed
    // cube glyph while it is queued or when there is nothing to show.
    void DrawMeshSlotThumbnail( Assets::AssetManager* manager, UI::UIHelper* uiHelper, Assets::AssetHandle mesh,
                                Assets::AssetHandle fallbackMaterial, float size = kMeshSlotThumbnailSize );
} // namespace Desert::Editor

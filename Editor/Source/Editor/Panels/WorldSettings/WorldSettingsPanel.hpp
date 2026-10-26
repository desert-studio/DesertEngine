#pragma once

#include <Editor/Core/IPanel.hpp>
#include "Editor/Widgets/UIHelper/ImGuiUI.hpp"

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <memory>
#include <optional>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // UE's World Settings (SLevelEditor.cpp:1246-1265): a Details view on the level's own settings
    // object. Every field of Core::SceneSettings is drawn by ONE PropertyEditorBuilder pass over its
    // reflection, grouped by the fields' Category — so a field added to SceneSettings with a PROPERTY
    // appears here with its range, its asset picker and its undo, and nothing in this panel names it.
    // The AssetManager is what the asset slots (Splash Sprite, Default Pawn, Player Controller) pick
    // and resolve through; without it they draw as bare handles.
    //
    // The one section that is not a setting is "Shadow Maps": an INSPECTOR that reads the renderer's
    // cascade images and writes nothing (К2 moved the shadow debug flag to the viewport's View Mode).
    class WorldSettingsPanel final : public IPanel
    {
    public:
        WorldSettingsPanel( std::shared_ptr<::Desert::Core::Scene>          scene,
                            std::shared_ptr<::Desert::Assets::AssetManager> assetManager );

        void OnUIRender() override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        std::shared_ptr<::Desert::Core::Scene>          m_Scene;
        std::shared_ptr<::Desert::Assets::AssetManager> m_AssetManager;
        std::unique_ptr<UI::UIHelper> m_UIHelper; // texture-slot thumbnails + the CSM cascade depth maps

        // The partition as it was when the Chunk Region Size drag began: the undo entry's "before".
        std::optional<::Desert::Core::WorldPartitionSerialized> m_ChunkRegionBefore;
    };
} // namespace Desert::Editor

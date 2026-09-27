#pragma once

#include <array>

#include "../IPanel.hpp"

#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Engine/ECS/Components.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief The Landscape mode's tool panel — UE's SLandscapeEditor: the mode row, the tool strip (icon + name,
     *        the current tool lit), then the Details sections Tool Settings and Brush Settings for the CURRENT
     * tool.
     *
     * Contextual like the Modeling panel: it exists while the viewport is in Landscape mode. Every value it edits
     * comes from LandscapeSculptState's tables, which the command palette offers too.
     */
    class LandscapePanel final : public IPanel
    {
    public:
        /// The scene is a constructor argument, like the Modeling and Scene Settings panels': SetScene only
        /// runs when the ACTIVE scene changes, and the primary scene is already active at registration.
        explicit LandscapePanel( const std::shared_ptr<Desert::Core::Scene>& scene );

        void OnUIRender() override;
        bool IsContextual() const override
        {
            return true;
        }
        bool IsRelevant() const override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        void DrawNewLandscape();
        void DrawHeightmapFile();
        void DrawToolStrip();
        void DrawToolSettings();
        void DrawBrushSettings();
        void DrawPaintSettings();
        void DrawTargetLayers();

        /// The scene the Target Layers list edits (EditorLayer rebinds it to the focused viewport's scene).
        std::weak_ptr<Desert::Core::Scene> m_Scene;
        /// UE's Import / Export file field; a relative path is under the project's Assets folder.
        std::array<char, 512> m_HeightmapPath{ "Landscape/Heightmaps/Landscape.png" };
        /// The layer list as it was when the current widget edit began; one undo entry per finished edit.
        // The layer info being edited (Hardness / No Weight Blend / swatch): a copy written to its `.delayerinfo`
        // when the widget is released, not every frame of a drag.
        std::optional<std::pair<Assets::AssetHandle, Assets::Serialization::LandscapeLayerInfoData>> m_LayerEdit;
    };
} // namespace Desert::Editor

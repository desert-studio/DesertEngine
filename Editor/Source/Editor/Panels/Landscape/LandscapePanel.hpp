#pragma once

#include "../IPanel.hpp"

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
        LandscapePanel();

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
        void DrawToolStrip();
        void DrawToolSettings();
        void DrawBrushSettings();
        void DrawPaintSettings();
        void DrawTargetLayers();

        /// The scene the Target Layers list edits (EditorLayer rebinds it to the focused viewport's scene).
        std::weak_ptr<Desert::Core::Scene> m_Scene;
        /// The layer list as it was when the current widget edit began; one undo entry per finished edit.
        std::optional<std::vector<ECS::LandscapeLayerInfo>> m_LayersEditStart;
    };
} // namespace Desert::Editor

#pragma once

#include "../IPanel.hpp"

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

    private:
        void DrawToolStrip();
        void DrawToolSettings();
        void DrawBrushSettings();
    };
} // namespace Desert::Editor

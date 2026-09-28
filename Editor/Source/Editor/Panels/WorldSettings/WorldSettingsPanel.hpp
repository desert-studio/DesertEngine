#pragma once

#include "../IPanel.hpp"
#include "Editor/Widgets/UIHelper/ImGuiUI.hpp"

#include <memory>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // Live editor for the LEVEL's own policy — the render path, its screen-space effects, shadows, the
    // grade, the lens, wind — plus the handful of MACHINE quality knobs that sit next to them because a
    // person tuning a picture reaches for both in one sitting. Edits are in place and take effect on the
    // next BeginScene; nothing here has an apply step.
    //
    // TWO STORES ARE REACHED FROM THIS ONE PANEL, AND EVERY CONTROL SAYS WHICH.
    //   * "(scene)"        — Core::SceneSettings, on the mutable reference below. Saved when the LEVEL is.
    //   * "(this machine)" — Common::Settings::MachineSettings, committed to that file on the click, the
    //                        way the viewport's Show flags commit to editor.json.
    // The suffix is not decoration: this panel drew the two next to each other for a long time with only
    // the AA pair labelled, and the unlabelled ones were where К3 found five machine settings living in
    // the level file. A control here without a suffix is one whose owner nobody could plausibly doubt
    // (a bloom threshold, a wind direction); a quality knob without one is a bug.
    //
    // WHAT THIS HEADER USED TO CLAIM AND THE PANEL NO LONGER HOLDS: "outline" and "debug". К2 moved the
    // selection outline to Editor::EditorPreferences and the ten debug-visualization flags to
    // Graphic::DebugViewState, and the Shadow Maps section that survived is an INSPECTOR (it reads GPU
    // images and writes nothing) rather than settings. A comment promising what the tree does not hold is
    // the same defect as dead code.
    class WorldSettingsPanel final : public IPanel
    {
    public:
        explicit WorldSettingsPanel( std::shared_ptr<::Desert::Core::Scene> scene );

        void OnUIRender() override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        std::unique_ptr<UI::UIHelper>          m_UIHelper; // for the CSM cascade depth-map thumbnails
    };
} // namespace Desert::Editor

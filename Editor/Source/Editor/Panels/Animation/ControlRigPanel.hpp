#pragma once

#include "../IPanel.hpp"

#include <memory>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    /**
     * @brief The selected entity's CONTROL RIG: what it has, which control is selected, and what it drives.
     *
     * View -> Control Rig. Minimal on purpose: see the controls, select one, move it. The moving happens in
     * the VIEWPORT, because a drag needs a pointer and a camera — `LightGizmoRenderer::RenderControlRig`
     * is the caller `ControlManipulator` was written for — and this panel is the half that answers "which
     * one" and "what is it attached to".
     *
     * THE TWO HALVES SHARE `Core::ControlRigEditMode` AND NOTHING ELSE. Neither owns the other: the panel
     * can be closed with the overlay still drawing, and the overlay's click changes the panel's highlight,
     * because the selection is a third thing both read. That is the same arrangement the bone tree and the
     * skeleton overlay already have.
     *
     * IT SHOWS THE STAGE, NOT THE FILE. The list is read out of the live `ControlRigStage` the entity's
     * Animator holds — the one `AnimationECSSystem` built from the `.derig` against THIS entity's skeleton.
     * Reading the file instead would show controls whose bone spaces may not have resolved, which is
     * precisely the difference between "the rig is in the project" and "the rig is posing this character".
     */
    class ControlRigPanel final : public IPanel
    {
    public:
        explicit ControlRigPanel( std::shared_ptr<::Desert::Core::Scene> scene );

        ImVec2 GetDefaultSize() const override
        {
            return ImVec2( 320.0f, 380.0f );
        }
        void OnUIRender() override;

        // Contextual: a rig belongs to a skinned mesh with an Animator.
        // NOT contextual: selecting a character is not a request to start posing it. It opens only when
        // asked for — the View menu or the command palette.
        bool IsContextual() const override
        {
            return false;
        }
        bool IsRelevant() const override;

    private:
        std::shared_ptr<::Desert::Core::Scene> m_Scene;
    };
} // namespace Desert::Editor

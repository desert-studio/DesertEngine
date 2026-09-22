#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/Selection/AuthoringContext.hpp>

#include <functional>
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

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 320.0f, 380.0f };
        }
        void OnUIRender() override;

        // Contextual: a rig belongs to a skinned mesh with an Animator.
        // NOT contextual: selecting a character is not a request to start posing it. It opens only when
        // asked for — the View menu or the command palette.
        [[nodiscard]] bool IsContextual() const override
        {
            return false;
        }
        [[nodiscard]] bool IsRelevant() const override;

    private:
        // WHO THIS PANEL IS when it writes the context, and its own durable copy of it. Built once for
        // the same reason the viewport's is: the owner is compared by value on every write.
        const Core::AuthoringOwner m_AuthoringOwner = Core::AuthoringOwner::ForPanel( "Control Rig" );
        Core::AuthoringContext     m_Authoring;

        // Take the context for @p entity and run @p write against it, logging a refusal rather than
        // dropping it. One place, because all four of this panel's controls do exactly this.
        void Author( const Common::UUID& entity, const char* what,
                     const std::function<Common::BoolResultStr( Core::AuthoringContext& )>& write );

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
    };
} // namespace Desert::Editor

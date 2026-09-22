#pragma once

#include "../IPanel.hpp"

#include <entt/entt.hpp>

#include <memory>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // View -> UI Debugger. What the UI canvas did this frame: how many batches and draw calls it cost and
    // WHY each batch was opened, how the walk spent the element tree, and everything about the selected
    // element — its resolved rect, its anchors, whether it is drawn and what stopped it, the pixels it may
    // occupy after clipping, its place in draw order, and (on request) the geometry and the batch it costs.
    //
    // IT OWNS NO NUMBERS. Every figure comes out of UI::UIFrameProbe, which the scene's EditorUIPass fills
    // from the very DrawList2D it is about to flush — so the panel cannot disagree with the renderer, and
    // every figure it shows is asserted by Desert/Tests/Engine/UIIntrospection rather than by looking at
    // this window. That order is deliberate: a panel whose only evidence is a screenshot was shipped here
    // before and had to be rebuilt.
    //
    // WHILE IT IS CLOSED IT COSTS ONE BRANCH PER FRAME. Opening it arms the probe; closing it disarms it
    // and drops the captured frame, because a reading from a frame that has since been redrawn still looks
    // like evidence.
    class UIDebuggerPanel final : public IPanel
    {
    public:
        explicit UIDebuggerPanel( std::shared_ptr<::Desert::Core::Scene> scene );
        ~UIDebuggerPanel() override;

        glm::vec2 GetDefaultSize() const override
        {
            return { 560.0f, 520.0f };
        }

        void SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene ) override
        {
            m_Scene    = scene;
            m_Measured = entt::null; // an entity id is unique only inside its own registry
        }

        // Arms and disarms the probe. OnPreUpdate and not OnUIRender: the latter is not called while the
        // window is closed, so it can arm but can never disarm.
        void OnPreUpdate() override;
        void OnUIRender() override;

    private:
        std::shared_ptr<::Desert::Core::Scene> m_Scene;

        // The probe is armed while this is true and disarmed the frame it stops being true — including
        // when the panel is destroyed, which is why the destructor is written out.
        bool m_Arming = false;

        bool m_ShowBatchList = true;

        // The element the cost measurement was last asked for, so a new selection asks once and a
        // steady one does not ask again. Cleared when the panel is pointed at another scene — an
        // entity id means nothing outside the registry it came from.
        entt::entity m_Measured = entt::null;
    };
} // namespace Desert::Editor

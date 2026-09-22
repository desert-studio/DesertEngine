#pragma once

#include "../IPanel.hpp"

#include <memory>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Animation
{
    class AnimationLibrary;
}

namespace Desert::Editor
{
    // Manages the animation LAYERS of the selected skinned-mesh entity's Animator: lists active layers
    // (clip, weight, additive, remove) and adds new ones (override / additive, optional bone mask). The
    // base clip is driven from the Sequencer / Details; this panel is the layer stack on top of it.
    // View -> Anim Layers.
    class AnimLayersPanel final : public IPanel
    {
    public:
        AnimLayersPanel( std::shared_ptr<::Desert::Core::Scene> scene, Animation::AnimationLibrary* library );

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 340.0f, 300.0f };
        }
        void OnUIRender() override;

        // Contextual: layers apply to a skinned mesh.
        // NOT contextual: selecting an object is not a request to open the layer stack. It opens only when
        // asked for — a button in Details, the View menu, or the command palette (Core::PanelRequests).
        bool IsContextual() const override
        {
            return false;
        }
        bool IsRelevant() const override;

    private:
        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        Animation::AnimationLibrary*           m_Library = nullptr;

        int   m_AddClip         = -1;
        float m_AddWeight       = 1.0f;
        bool  m_AddAdditive     = false;
        char  m_AddMaskBone[64] = {};
    };
} // namespace Desert::Editor

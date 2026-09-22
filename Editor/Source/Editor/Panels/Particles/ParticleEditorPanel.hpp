#pragma once

#include "../IPanel.hpp"

#include <Common/Core/UUID.hpp>

#include <memory>
#include <string>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::ECS
{
    struct ParticleEmitterComponent;
}

namespace Desert::Editor
{
    // ── ONE ENTITY'S PARTICLE EMITTER: A DOCUMENT, NOT A TOOL ─────────────────────────────────────────
    //
    // A friendlier home than the flat auto-Details for ONE ParticleEmitterComponent: one-click presets
    // (Fire / Smoke / Sparks / Magic / Explosion), a colour-over-life gradient bar, a size-over-life curve
    // plot driven by the ease-power, grouped emission/motion controls and a live stats readout.
    //
    // IT USED TO BE A SINGLETON THAT FOLLOWED THE SELECTION. Four of its five possible screens were empty
    // states — no scene, nothing selected, selection not found, no emitter on it — because a window that
    // is about "whatever is selected" has to have an opinion about every way that can be nothing. Its
    // RequestOpen inbox had NO CALLERS AT ALL: the Details button its own header promised was never built,
    // because there was nothing for the button to say. Now there is, and it is the subject.
    //
    // THE SUBJECT IS A COMPONENT, NOT A FILE. There is no particle-system asset; the emitter's data lives
    // in the component and is serialized with the scene. See Editor/Core/EditorSubject.hpp.
    class ParticleEditorPanel final : public ISubjectDocument
    {
    public:
        // The subject type this editor is registered under — one literal, read by the registration and by
        // the Details button, because two that must agree is the shape that drifts.
        static constexpr const char* kComponentTypeName = "ParticleEmitterComponent";

        [[nodiscard]] static SubjectTypeKey SubjectType()
        {
            return ComponentSubjectType( kComponentTypeName );
        }

        [[nodiscard]] static SubjectId SubjectFor( const Common::UUID& entity )
        {
            return ComponentSubject( entity, kComponentTypeName );
        }

        ParticleEditorPanel( const SubjectId& subject, const std::string& displayName,
                             const std::shared_ptr<::Desert::Core::Scene>& scene );

        glm::vec2 GetDefaultSize() const override
        {
            return { 460.0f, 620.0f };
        }
        void OnUIRender() override;

        // The entity, in the scene this document was opened over, still carrying a ParticleEmitterComponent.
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return ResolveComponent() != nullptr;
        }

        // NOTHING BUT IMGUI. The gradient bar and the curve plot are drawn with ImGui primitives; there is
        // no Scene, no SceneRenderer and no offscreen target here, so this window is not demand for one of
        // the six slots and closing it would free nothing.
        [[nodiscard]] bool HoldsRendererSlot() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsRendererSlot() const override
        {
            return false;
        }

        // DELIBERATELY A NO-OP. It used to rebind to the newly focused scene, which is right for a tool and
        // wrong for a document: the subject is an entity UUID, and UUIDs belong to one registry — following
        // the fanout would point this window at a scene where the id names nothing, or names somebody else.
        // See AnimGraphPanel::SetScene, which says the same thing at more length.
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& /*scene*/ ) override
        {
        }

    private:
        // The component this window edits, or nullptr when it is gone. ONE resolution, used by the draw and
        // by the liveness answer, so the two cannot disagree.
        [[nodiscard]] ECS::ParticleEmitterComponent* ResolveComponent() const;

        // WEAK, not shared — see AnimGraphPanel for the argument. A closed scene is one of the ways this
        // document's subject dies, and a strong reference would hide that and leak the level with it.
        std::weak_ptr<::Desert::Core::Scene> m_Scene;
    };
} // namespace Desert::Editor

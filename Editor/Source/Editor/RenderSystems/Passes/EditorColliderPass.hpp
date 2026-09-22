#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp>

namespace Desert::Editor::Render
{
    // TRUE-3D physics-collider wireframes (Box / Sphere / Capsule) drawn through the Editor Pass API:
    // a Debug-phase line pass into the scene HDR target, depth-tested against geometry — unlike the old
    // ImGui screen-space projection, colliders now occlude correctly behind walls. Shapes mirror exactly
    // what PhysicsECSSystem feeds Jolt: entity translation+rotation, WORLD units, no scale.
    // Gated by the VIEW's own Graphic::DebugViewState::ShowColliders (SceneRenderer::GetDebugView),
    // not by scene data — К2 took the flag out of the level file. Hidden in Play mode.
    class EditorColliderPass
    {
    public:
        ~EditorColliderPass();

        // (Re)creates the line pipeline against the scene's CURRENT target framebuffer and registers
        // the pass. Call after every Scene::Init.
        Common::BoolResultStr Install( const std::shared_ptr<::Desert::Core::Scene>& scene );

    private:
        void BuildLines( std::vector<Graphic::MaterialDebugLine::LineVertex>& outLines ) const;

        std::weak_ptr<::Desert::Core::Scene>        m_Scene;
        std::shared_ptr<Graphic::GraphicsPipeline>  m_Pipeline;
        std::unique_ptr<Graphic::MaterialDebugLine> m_Material;
    };
} // namespace Desert::Editor::Render

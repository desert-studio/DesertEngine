#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp>

#include <cstdint>

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
        // Volumes always (UE: SHOW_Volumes is on in the editor, the brush outline is all a volume shows); every
        // other collider only with the view's Colliders toggle.
        void BuildLines( std::vector<Graphic::MaterialDebugLine::LineVertex>& outLines, bool showColliders ) const;

        std::weak_ptr<::Desert::Core::Scene>        m_Scene;
        std::shared_ptr<Graphic::GraphicsPipeline>  m_Pipeline;
        std::unique_ptr<Graphic::MaterialDebugLine> m_Material;
        // The vertices this view's Declare uploaded (0 = nothing to draw).
        uint32_t m_LineVertices = 0;
    };
} // namespace Desert::Editor::Render

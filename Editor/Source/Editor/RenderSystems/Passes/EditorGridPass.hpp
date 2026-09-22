#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialGrid.hpp>

namespace Desert::Editor::Render
{
    // The infinite ground-plane grid — an AUTHORING aid, so it lives in the editor and reaches the
    // scene through the Editor Pass API (Scene::RegisterExternalPass): a Transparency-phase pass into
    // the scene HDR target, depth-occluded by geometry, alpha-blended before the post chain. Hidden in
    // Play mode and via the VIEW's own Graphic::DebugViewState::ShowGrid (SceneRenderer::GetDebugView),
    // which is where the flag lives since К2 took it out of the level file.
    class EditorGridPass
    {
    public:
        ~EditorGridPass();

        // (Re)creates the pipeline against the scene's CURRENT target framebuffer and registers the
        // pass. Call after every Scene::Init — the framebuffers are recreated there.
        Common::BoolResultStr Install( const std::shared_ptr<::Desert::Core::Scene>& scene );

    private:
        std::weak_ptr<::Desert::Core::Scene>       m_Scene;
        std::shared_ptr<Graphic::GraphicsPipeline> m_Pipeline;
        std::unique_ptr<Graphic::MaterialGrid>     m_Material;
    };
} // namespace Desert::Editor::Render

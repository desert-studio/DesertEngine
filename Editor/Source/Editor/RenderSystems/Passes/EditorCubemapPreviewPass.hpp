#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialCubemapSphere.hpp>

#include <functional>

namespace Desert::Editor::Render
{
    // The Material Editor preview's cubemap presenter: a Debug-phase external pass that ray-traces a
    // ball at the origin and wraps a cubemap onto it (shader "CubemapSphere" — see it for why the ball
    // is a fullscreen quad and why nothing lights it).
    //
    // WHY A PASS AND NOT A MESH IN THE PREVIEW SCENE. The mesh path draws Surface-domain materials
    // only (MeshRenderer::DrawGenericMeshes refuses everything else BY NAME, and rightly), so a
    // Skybox-domain material cannot ride a StaticMeshComponent slot. The alternative — a scratch
    // Surface material mirroring the subject's cubemap — is a copy that has to be kept in step with
    // every edit, the exact middle-link defect the slot route was built to end. This pass holds no
    // copy at all: it RE-RESOLVES the cube through the closure every frame, so a texture dropped onto
    // the material shows the next frame with no invalidation protocol.
    //
    // The closure seam is also the preview's extension point working as designed: a domain answers
    // "what fills the pane" with its own draw (Surface: a lit primitive through the slot route; this
    // domain: a wrapped ball; a future Volume domain: a march), not with a shape from a list.
    class EditorCubemapPreviewPass
    {
    public:
        ~EditorCubemapPreviewPass();

        // (Re)creates the pipeline against the scene's CURRENT target framebuffer and registers the
        // pass. Call after every Scene::Init — the framebuffers are recreated there.
        Common::BoolResultStr Install( const std::shared_ptr<::Desert::Core::Scene>& scene );

        // What to show: the cube resolved fresh each frame, and the ball's radius in world units.
        // A null resolver — or a resolver answering null — draws nothing (the pane's refusal text is
        // the panel's job; a silent black ball here would bury it).
        void SetSource( std::function<Graphic::SampledCube()> resolveCube, float radiusWorldUnits )
        {
            m_ResolveCube = std::move( resolveCube );
            m_Radius      = radiusWorldUnits;
        }

        void ClearSource()
        {
            m_ResolveCube = nullptr;
        }

    private:
        std::weak_ptr<::Desert::Core::Scene>            m_Scene;
        std::shared_ptr<Graphic::GraphicsPipeline>      m_Pipeline;
        std::unique_ptr<Graphic::MaterialCubemapSphere> m_Material;
        std::function<Graphic::SampledCube()>           m_ResolveCube;
        float                                           m_Radius = 50.0f;
    };
} // namespace Desert::Editor::Render

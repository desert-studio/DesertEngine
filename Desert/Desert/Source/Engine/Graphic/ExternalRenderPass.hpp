#pragma once

#include <Engine/Graphic/RenderGraphBuilder.hpp>
#include <Engine/Graphic/Pipeline.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Camera;
}

namespace Desert::Graphic
{
    class Framebuffer;
    class Image2D;
    class SceneRenderer;

    // Per-frame data handed to an external (editor-registered) pass when the render graph executes it.
    // The pass draws into Target — the HDR scene framebuffer with the geometry depth attached — so its
    // output is depth-occluded by the scene and feeds the normal post-process chain.
    struct ExternalPassContext
    {
        const Core::Camera* Camera       = nullptr; // THIS VIEW's camera (editor or gameplay)
        Framebuffer*        Target       = nullptr; // HDR scene target the pass is drawing into
        Image2D*            Depth        = nullptr; // scene depth attachment (bound for depth test)
        bool                ScenePlaying = false;   // true in Play mode — authoring aids usually hide
        // THE VIEW THIS PASS IS DRAWING INTO. A scene has a LIST of views now (Engine/Core/SceneViewList.hpp)
        // and a pass runs once per view, so "what is this view showing" — the debug/show flags, the
        // render path — is a per-renderer question. Every editor pass used to ask it of
        // `scene->GetSceneRenderer()`, which answers for the FIRST view only; its own comment already
        // claimed it was "asked of the RENDERER this pass is drawing into", and this is the field that
        // makes that true.
        SceneRenderer* Renderer = nullptr;
    };

    // A render pass injected from outside the engine (the editor's debug-draw tools). Phase +
    // dependencies place it in the scene render graph — the default slots it after Geometry, blended
    // over the lit scene and before post-processing, exactly where authoring aids belong. The owner
    // creates its own pipeline against Scene::GetTargetFramebuffer() and records draws (e.g.
    // Renderer::SubmitFullscreenQuad) inside Execute; the graph opens/closes the render pass around it.
    struct ExternalPassSpecification
    {
        std::string   Name;
        RenderPhaseID Phase = RenderPhase::Debug;

        std::vector<RenderPassDependency> Dependencies = { RenderPassDependency( RenderPhase::Geometry ) };

        // Render-state the graph uses for the cached VkRenderPass of this pass (usually the
        // specification of the pipeline the owner will draw with).
        GraphicsPipelineSpecification PipelineSpecification;

        std::function<void( const ExternalPassContext& )> Execute;
    };
} // namespace Desert::Graphic

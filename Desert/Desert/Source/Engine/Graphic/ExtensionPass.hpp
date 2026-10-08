#pragma once

#include <Engine/Graphic/RDG/RDGExtensionPoint.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>

#include <functional>
#include <string>

namespace Desert::Core
{
    class Camera;
}

namespace Desert::Graphic
{
    class Framebuffer;
    class Image2D;
    class SceneRenderer;

    // Per-frame data handed to an extension pass (the editor's grid, colliders, UI canvas...) when the render
    // graph executes it. The pass draws into Target - the HDR scene framebuffer with the geometry depth attached
    // (after the temporal resolve, the graph substitutes the output-extent overlay set for it) - so its output is
    // depth-occluded by the scene and feeds the normal post-process chain.
    struct ExtensionPassContext
    {
        const Core::Camera* Camera       = nullptr; // THIS VIEW's camera (editor or gameplay)
        Framebuffer*        Target       = nullptr; // HDR scene target the pass is drawing into
        Image2D*            Depth        = nullptr; // scene depth attachment (bound for depth test)
        bool                ScenePlaying = false;   // true in Play mode — authoring aids usually hide
        // THE VIEW THIS PASS IS DRAWING INTO. A scene has a LIST of views (Engine/Core/SceneViewList.hpp) and a
        // pass runs once per view, so "what is this view showing" — the debug/show flags, the render path — is a
        // question for the renderer drawing it, never for `scene->GetSceneRenderer()` (the FIRST view only).
        SceneRenderer* Renderer = nullptr;
        // This frame's graph refs (FrameGraphRefs): Declare names the graph textures it samples by them
        // (RenderPassDeclaration::Read( RDG::TextureRef, ... )) and Execute binds them by shader name
        // (RDG::PassBindings over the RDG::PassContext it is given). E.g. the UI glass reads
        // Graph.Transients.BackdropBlur.
        FrameGraphRefs Graph;
    };

    // A render pass from outside the engine's frame build (UE: a scene view extension), placed by its
    // RDG::ExtensionPoint: SceneRenderer adds every pass registered at a point, in registration order, where its
    // frame build invokes that point (SceneRenderer::AddExtensionPoint). The owner creates its own pipeline
    // against Scene::GetTargetFramebuffer() and records draws (e.g. Renderer::DrawFullscreen with PassBindings)
    // inside Execute; the graph opens/closes the render pass around it, LOADing the target. Registered on the
    // Scene (Scene::RegisterExtensionPass), which every view of it reads each frame.
    struct ExtensionPass
    {
        std::string         Name;
        RDG::ExtensionPoint Point = RDG::ExtensionPoint::Overlay;

        // The pass body: @p pass is this node's RDG::PassContext (PassBindings / Renderer::DrawIndexed etc.).
        std::function<Common::BoolResultStr( const ExtensionPassContext&, RDG::PassContext& pass )> Execute;

        // What the pass samples besides its target (the scene framebuffer, declared whole by the graph): each
        // graph texture its shaders read, e.g. the UI's backdrop pyramid (ctx.Graph.Transients.BackdropBlur).
        // It runs while the frame graph is built, with the same context Execute gets. Leave it empty when the
        // pass reads nothing but what it draws over.
        std::function<void( RenderPassDeclaration&, const ExtensionPassContext& )> Declare;
    };

    using ExtensionPassRegistry = RDG::ExtensionRegistry<ExtensionPass>;
} // namespace Desert::Graphic

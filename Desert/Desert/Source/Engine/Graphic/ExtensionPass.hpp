#pragma once

#include <Engine/Graphic/RDG/RDGExtensionPoint.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>

#include <glm/glm.hpp>

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

    // What an extension pass is handed when its view OPENS its frame (SceneRenderer::BeginScene), on the GAME
    // thread. UE: ISceneViewExtension::SetupView / BeginRenderViewFamily - whatever the extension reads from the
    // world is collected into its frame data here, and the render thread sees only that copy.
    struct ExtensionPassGatherContext
    {
        const Core::Camera* Camera       = nullptr; // THIS VIEW's camera, frozen for the frame (FrozenCamera)
        SceneRenderer*      Renderer     = nullptr; // the view this frame is for (see ExtensionPassContext)
        bool                ScenePlaying = false;   // true in Play mode - authoring aids usually hide
        // The view's OUTPUT extent: the display-referred final image the UI point draws into, which is also the
        // render area its node gets (RenderPassDeclaration::TargetExtent), never the render-scale extent.
        glm::uvec2 OutputExtent{ 0u, 0u };
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

        // WHAT THE PASS READS FROM THE WORLD, read on the GAME thread when the view opens its frame
        // (SceneRenderer::BeginScene, once per view that draws the extension points). Declare and Execute run
        // while the frame graph is built and executed - on the render task (Engine/Core/RenderTask.hpp), while
        // the interface edits the registry, moves the live camera and talks to the OS - so they read only what
        // Gather left in the pass: never the registry, ImGui, the window or a panel's state. BeginScene joins the
        // previous view's frame before it gathers, so the pass's gathered state is one view's at a time. Leave it
        // empty when Declare and Execute read nothing but their context and the pass's own pipeline.
        std::function<void( const ExtensionPassGatherContext& )> Gather;
    };

    using ExtensionPassRegistry = RDG::ExtensionRegistry<ExtensionPass>;
} // namespace Desert::Graphic

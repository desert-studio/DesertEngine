#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Graphic/Render2D/Render2D.hpp>
#include <Engine/Graphic/Render2D/UIRenderTextureCache.hpp>
#include <Engine/UI/UICanvasContext.hpp>

namespace Desert::Editor::Render
{
    // Draws the scene's UI canvas with the engine's own 2D batcher (Render2D) instead of ImGui — a
    // UI-phase pass into the scene HDR target, composited on top via a load pass. Installed like the
    // grid, through the Editor Pass API (Scene::RegisterExternalPass). This is the migration vehicle:
    // it starts with flat-colour panels and grows toward full parity, after which the ImGui UI overlay
    // is retired.
    class EditorUIPass
    {
    public:
        ~EditorUIPass();

        // (Re)creates the Render2D pipeline against the scene's CURRENT target framebuffer and registers
        // the pass. Call after every Scene::Init — the framebuffers are recreated there.
        Common::BoolResultStr Install( const std::shared_ptr<Core::Scene>& scene );

        // ADVANCE THE WORLDS THIS VIEW'S RENDER-TEXTURE ELEMENTS SHOW (Ю16). MUST be called from the
        // editor's pre-update, before any scene of this frame opens a pass: a capture records a whole
        // scene render, and Vulkan has no nested render pass — the pass below runs INSIDE one. It is a
        // method on the pass rather than work the pass does itself for exactly that reason; the two
        // halves are described in UIRenderTextureCache.hpp.
        void TickRenderTextures( Assets::AssetManager& assetManager, const Common::Timestep& ts )
        {
            m_RenderTextures.Tick( assetManager, ts );
        }

    private:
        std::weak_ptr<Core::Scene>  m_Scene;
        Graphic::Render2D::Render2D m_Render2D;

        // The UI runtime state of THIS viewport — one cell per (canvas x this view). One EditorUIPass
        // exists per open scene document (Render::RenderRegistry builds one in its constructor), so this is
        // what keeps two viewports from sharing a hover clock, an elected hot element or a screen stack,
        // and the cells inside it are what keeps the level's own canvases from sharing them with each
        // other. It also drives the scene's UIAnim playheads — the UI Editor panel's preview deliberately
        // does not, or a clip would advance twice a frame.
        //
        // LIFETIME: by value in the pass, and the pass is owned by the document's RenderRegistry. Closing
        // the document destroys the pass and with it every cell — there is nothing to release by hand.
        ::Desert::UI::UIViewContext m_UIView;

        // The offscreen worlds behind this view's render-texture elements. By value in the pass for the
        // same reason m_UIView is: closing the document destroys the pass, and destroying this is what
        // hands its renderer slots back — there is nothing to release by hand, and nothing that can be
        // forgotten at a close site.
        Graphic::Render2D::UIRenderTextureCache m_RenderTextures;
    };
} // namespace Desert::Editor::Render

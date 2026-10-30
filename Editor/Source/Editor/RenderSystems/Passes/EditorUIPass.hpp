#pragma once

#include <UI/Retained/RetainedCanvasResources.hpp>
#include <UI/Retained/WidgetGallery.hpp>
#include <UI/Retained/WidgetTree.hpp>
#include <Engine/UI/Ecs/RegistryUICanvasResources.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Graphic/Render2D/Render2D.hpp>
#include <Engine/Graphic/Render2D/UIRenderTextureCache.hpp>
#include <UI/UICanvasContext.hpp>
#include <UI/UICanvasRenderer2D.hpp>

namespace Desert::Editor::Render
{
    // Draws the scene's UI canvas with the engine's own 2D batcher (Render2D) instead of ImGui — a
    // UI-phase pass into the scene HDR target, composited on top via a load pass. Installed like the
    // grid, through the Editor Pass API (Scene::RegisterExtensionPass). This is the migration vehicle:
    // it starts with flat-colour panels and grows toward full parity, after which the ImGui UI overlay
    // is retired.
    class EditorUIPass
    {
    public:
        ~EditorUIPass();

        // (Re)creates the Render2D pipeline against the scene's CURRENT target framebuffer and registers
        // the pass. Call after every Scene::Init — the framebuffers are recreated there.
        Common::BoolResultStr Install( const std::shared_ptr<::Desert::Core::Scene>& scene );

        // ADVANCE THE WORLDS THIS VIEW'S RENDER-TEXTURE ELEMENTS SHOW (Ю16). MUST be called from the
        // editor's pre-update, before any scene of this frame opens a pass: a capture records a whole
        // scene render, and Vulkan has no nested render pass — the pass below runs INSIDE one. It is a
        // method on the pass rather than work the pass does itself for exactly that reason; the two
        // halves are described in UIRenderTextureCache.hpp.
        void TickRenderTextures( Assets::AssetManager& assetManager, const Common::Timestep& ts )
        {
            m_RenderTextures.Tick( assetManager, ts );
        }

        // This frame's step, handed in by the editor before the scene's frame (RenderRegistry::BeginFrame).
        // The pass runs inside the render graph, which carries no time; the walk spends this delta.
        void BeginFrame( const Common::Timestep& ts )
        {
            m_FrameDtSeconds = ts.GetSeconds();
        }

    private:
        std::weak_ptr<::Desert::Core::Scene> m_Scene;

        // The step BeginFrame was last handed — what this view's UI frame advances by.
        float m_FrameDtSeconds = 0.0f;

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
        // The engine resources this view draws with; declared first, so it outlives the view built on it.
        ::Desert::UI::RegistryUICanvasResources m_UIResources;
        ::Desert::UI::UIViewContext             m_UIView{ m_UIResources };

        // The retained widget gallery (UIPreview::WidgetGallery): its own view over its own tree, drawing
        // with this pass's resources minus the scene's clips (RetainedCanvasResources). The model is declared
        // before the tree so it outlives the delegates that write it.
        ::Desert::UI::RetainedCanvasResources m_GalleryResources{ m_UIResources };
        ::Desert::UI::UIViewContext           m_GalleryView{ m_GalleryResources };
        ::Desert::UI::WidgetGalleryModel      m_GalleryModel;
        ::Desert::UI::WidgetTree              m_Gallery;
        ::Desert::UI::NodeId                  m_GalleryCanvas  = ::Desert::UI::NodeId::Null;
        ::Desert::UI::NodeId                  m_GalleryFocused = ::Desert::UI::NodeId::Null;
        // The one keyboard both views share (UIKeyboardUser): the level's canvases own it until a press lands
        // on the gallery, and get it back on a press on one of theirs or when the gallery is hidden.
        ::Desert::UI::UIKeyboardUser m_Keyboard{ .Owner = &m_UIView };
        // `--ui-press` under `--play`: walks seen with the scene in Play. The press waits for the second, whose
        // `Hot` was elected over the rows gameplay filled on the first (see the press gate in Declare).
        int m_CliPlayWalks = 0;

        // The offscreen worlds behind this view's render-texture elements. By value in the pass for the
        // same reason m_UIView is: closing the document destroys the pass, and destroying this is what
        // hands its renderer slots back — there is nothing to release by hand, and nothing that can be
        // forgotten at a close site.
        Graphic::Render2D::UIRenderTextureCache m_RenderTextures;
    };
} // namespace Desert::Editor::Render

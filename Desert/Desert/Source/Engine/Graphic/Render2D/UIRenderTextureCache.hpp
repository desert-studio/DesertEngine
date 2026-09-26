#pragma once

#include <Engine/UI/UIRenderTextureSource.hpp>

#include <Common/Core/Timestep.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Graphic
{
    class SceneRenderer;
}

namespace Desert::Graphic::Render2D
{
    /**
     * @brief The worlds behind one view's render-texture elements (Ю16).
     *
     * WHAT IT OWNS. One offscreen Core::Scene and one Graphic::SceneRenderer per element that is ON
     * SCREEN, keyed by the element entity. Each of those renderers is a view with its own GPU copies, and
     * the byte budget (Engine/Core/ViewBudget.hpp) decides whether one more fits.
     *
     * THE TWO HALVES, AND WHY THEY ARE TWO. Vulkan has no nested render pass, and the canvas walk runs
     * INSIDE one — in the editor it is an external pass on the scene's HDR target
     * (Editor/RenderSystems/Passes/EditorUIPass.cpp), in the runtime it is the swapchain pass. So a world
     * cannot be rendered where it is sampled, and the two halves are:
     *
     *   Tick()                  - from the host's PRE-UPDATE, before any pass opens. Destroys the captures
     *                             nothing demanded, builds the ones that are new, renders every live one.
     *   ResolveRenderTexture()  - from the walk, inside the pass. Samples what Tick left, and RECORDS THE
     *                             DEMAND for the next Tick.
     *
     * PreviewViewport::Update states the same ordering constraint for the Details preview and says why:
     * rendering from inside the UI pass destroys descriptor pools whose sets are bound to the recording
     * command buffer. This class is the same shape driven by the scene graph instead of by a panel.
     *
     * HOW A SLOT COMES BACK — the question the task turns on. A slot is returned by DESTROYING the
     * renderer that holds it and by nothing else; a flag saying "not now" would keep the lease. So
     * liveness is not tracked, it is OBSERVED: the walk asking about an element is the only evidence that
     * the element is on screen, and an element that was not asked about since the last Tick has its
     * capture destroyed. An element that scrolled out of a clipped list, had its Visible bit cleared, or
     * sits on a screen the stack is not showing is not walked — so it frees its slot with no code at any
     * of those sites.
     *
     * WHAT HAPPENS TO THE ONE PAST THE LAST SLOT. It is REFUSED, with the numbers in the log, and it
     * draws the magenta error fill — never nothing. WHICH element loses is decided by WALK ORDER, i.e.
     * authored order (canvas Sort Order, then depth-first): the elements the frame draws first get the
     * slots and the last one on the canvas is the one that goes without. That is a property of the scene
     * and not of a container — see m_Demanded for the measurement that made it one.
     *
     * Measured on Editor/Resources/Assets/Scenes/UI_RenderTextureBudget.desce, six elements against six
     * slots with the editor viewport already holding one: five build, the sixth is refused, and the log
     * names "all 6 of 6 renderer slots are in use" plus the two ways to get one back.
     *
     * WHAT IT DOES NOT ADD. No per-frame buffer, no descriptor set and no material of its own, so nothing
     * here is keyed by (frame x slot) — see Docs/RENDERER_FRAME_STATE.md for when that is required. The
     * picture is a Graphic::Image2D the capture's own SceneRenderer already keys that way, and the
     * descriptor set that samples it is Render2D's per-texture executor, which is keyed by the image
     * POINTER and retired by frame stamp (Render2D::ExecutorFor / RetireUnusedExecutors). Adding a second
     * cache here would have been a second owner for a lifetime that already has one.
     *
     * NESTING IS ONE LEVEL BY CONSTRUCTION. A capture scene's own canvases are never walked — only the
     * host view walks, and it walks its own registry — so a scene that contains a render-texture element
     * cannot recurse through this cache. A scene may therefore name itself without hanging; it renders a
     * second, independent copy of that world, which is what a camera pointed at a monitor showing the
     * same room actually looks like.
     */
    class UIRenderTextureCache final : public ::Desert::UI::IUIRenderTextureSource
    {
    public:
        UIRenderTextureCache();
        ~UIRenderTextureCache() override;

        UIRenderTextureCache( const UIRenderTextureCache& )            = delete;
        UIRenderTextureCache& operator=( const UIRenderTextureCache& ) = delete;

        /**
         * @brief Destroy what is no longer on screen, build what is newly on it, render what is live.
         *
         * MUST run from the host's pre-update, outside every render pass and before the frame's scene
         * render. @p assetManager is what the loaded scenes resolve their meshes and materials through;
         * @p ts is handed to each capture's own Scene::OnUpdate, so a world in a widget animates at the
         * host's rate rather than at a made-up one.
         */
        void Tick( Assets::AssetManager& assetManager, const Common::Timestep& ts );

        /// Destroy every capture and hand back every slot. Called when the view's scene changes: entity
        /// ids are unique only inside a registry, so a capture keyed by an id from the old one would be
        /// handed to whatever entity inherited that id.
        void Reset();

        /// How many captures are alive — i.e. how many renderer slots this view is holding. For the host
        /// that wants to say so, and for a test.
        [[nodiscard]] uint32_t LiveCaptureCount() const
        {
            return static_cast<uint32_t>( m_Captures.size() );
        }

        // IUIRenderTextureSource. The walk holds this object through the interface and never through the
        // concrete type — see UIRenderTextureSource.hpp for the link failure that rule was written from.
        [[nodiscard]] const void*
        ResolveRenderTexture( entt::entity element, const ::Desert::UI::UIRenderTextureRequest& request ) override;

    private:
        // One element's world. Destroying it is what returns the renderer slot, so this type is only ever
        // held by value in the map and erased.
        struct Capture
        {
            // DECLARATION ORDER IS THE DESTRUCTION CONTRACT, and it is backwards from how they are used:
            // members are destroyed in reverse, so Scene (built on the renderer) goes first and the
            // renderer that owns the slot goes second. Swapping these two frees the renderer under a live
            // scene.
            std::unique_ptr<SceneRenderer> Renderer;
            std::shared_ptr<Core::Scene>   Scene;

            std::string ScenePath; // what it was built from; a change rebuilds it
            uint32_t    Width  = 0;
            uint32_t    Height = 0;
        };

        // What the last walk asked for, one row per element it drew. Filled by ResolveRenderTexture,
        // consumed and cleared by Tick.
        struct Demand
        {
            entt::entity Element = entt::null;
            std::string  ScenePath;
            uint32_t     Width  = 0;
            uint32_t     Height = 0;
        };

        // Build one capture, or say why not. Returns nullptr on refusal, having logged the reason with
        // its numbers.
        [[nodiscard]] Capture* Build( entt::entity element, const Demand& demand,
                                      Assets::AssetManager& assetManager );

        std::unordered_map<entt::entity, Capture> m_Captures;

        // A VECTOR AND NOT A MAP, AND THE ORDER IS THE WHOLE REASON. Tick builds captures in the order it
        // reads this, and the first elements it reaches are the ones that get the scarce renderer slots —
        // so the container's iteration order IS the policy for who loses. An unordered_map made that
        // policy "whatever the hash says": measured on UI_RenderTextureBudget, six elements authored left
        // to right were served 5th, 4th, 3rd, 2nd, 1st and the FIRST one was the one refused, which is
        // both arbitrary and the opposite of what this class's own header claimed. Appended in walk
        // order, which is authored order (canvas Sort Order, then depth-first), so the element that loses
        // is the last one on the canvas and stays the last one on the canvas.
        //
        // Linear lookup is the right trade here and not a concession: a canvas cannot have more live
        // render-texture elements than there are renderer slots, so this vector is at most a handful of
        // rows and a std::find over it beats a hash.
        std::vector<Demand> m_Demanded;

        // Elements already reported as refused, so a shortage is one line per element per stretch rather
        // than one line per element per frame. Cleared for an element the moment it gets its capture.
        std::unordered_map<entt::entity, std::string> m_Refused;
    };
} // namespace Desert::Graphic::Render2D

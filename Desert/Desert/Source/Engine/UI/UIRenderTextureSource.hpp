#pragma once

#include <entt/entt.hpp>

#include <cstdint>
#include <string_view>

// WHERE A UI ELEMENT'S LIVE WORLD COMES FROM, stated as the one question the canvas walk asks and nothing
// more.
//
// WHY THIS IS AN INTERFACE AND NOT A POINTER TO THE CACHE — the same reason, measured, as
// UIMaterialSource.hpp next door. The walk (UICanvasRenderer2D) is pure: three suites compile it with no
// GPU, no Vulkan and no pipeline in the binary at all (UIIntrospection, UICanvasContext, UIEventRouting),
// which is what lets the batching, the hit test and the event routing be asserted rather than described.
// Naming Graphic::Render2D::UIMaterialCache in the context made those three fail to LINK on the one
// symbol the walk calls. A render texture is a whole Core::Scene and a Graphic::SceneRenderer behind it,
// so naming ITS backend would end that purity far more thoroughly than a material ever could.
//
// So the walk depends on the QUESTION and the backend supplies the ANSWER.
// Graphic::Render2D::UIRenderTextureCache is the only implementation; a suite that wants to test the
// walk's render-texture path supplies a stub.
namespace Desert::UI
{
    // What the walk knows about one render-texture element on the frame it is drawing it.
    //
    // THE ELEMENT'S SIZE IS PART OF THE QUESTION. The backend renders a world into an offscreen target,
    // and the only honest size for that target is the size the element is on screen this frame — which
    // the walk has already computed (anchors, canvas scale, layout groups) and the backend cannot
    // recompute without a second copy of the layout engine.
    struct UIRenderTextureRequest
    {
        // The scene to render, as UIRenderTextureData::ScenePath spelled it. A view of the component's
        // own string: it lives until the end of the walk, which is longer than this call.
        std::string_view ScenePath;

        // The offscreen target's size in pixels — the element's on-screen rect with ResolutionScale
        // already applied and clamped by the walk. Never zero: the walk skips a degenerate rect before
        // it asks.
        uint32_t WidthPx  = 0;
        uint32_t HeightPx = 0;
    };

    class IUIRenderTextureSource
    {
    public:
        virtual ~IUIRenderTextureSource() = default;

        // The opaque texture id (engine Image2D*) holding @p element's world as of this frame, or nullptr
        // when there is none to sample.
        //
        // NULL IS A REFUSAL AND IT IS ALREADY IN THE LOG. An element that renders nothing is
        // indistinguishable from an element that was meant to render nothing (§1.4), so the caller draws
        // the magenta error fill for a null — and the implementation owes the log the REASON with its
        // numbers before returning one. The reason belongs to the implementation and not to this
        // signature because only the implementation knows it: how many renderer slots are live, whether
        // the file parsed, which scene refused. The picture is what keeps saying it every frame; the log
        // line is said once per stretch, the same way UICanvasRenderer2D::ResolveUIMaterial does it.
        //
        // CALLING THIS IS ALSO THE DEMAND. An implementation that owns renderer slots learns from this
        // call, and only from this call, that the element is ON SCREEN — a slot is given back by
        // DESTROYING the capture, and nothing else can tell the backend that an element scrolled out of a
        // clipped list or had its Visible bit cleared. So a walk that skips an element is a walk that
        // releases it, with no discipline required at the skip site.
        [[nodiscard]] virtual const void* ResolveRenderTexture( entt::entity                  element,
                                                                const UIRenderTextureRequest& request ) = 0;
    };
} // namespace Desert::UI

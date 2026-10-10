#pragma once

#include <Common/Core/AssetHandle.hpp>

// WHERE A UI ELEMENT'S MATERIAL COMES FROM, stated as the one question the canvas walk asks and nothing
// more.
//
// WHY THIS INTERFACE EXISTS AND IS NOT JUST A POINTER TO THE CACHE. The walk (UICanvasRenderer2D) is
// pure: three suites compile it with no GPU, no Vulkan and no pipeline in the binary at all
// (UIIntrospection, UICanvasContext, UIEventRouting), which is what lets the batching, the hit test and
// the event routing be asserted rather than described. Naming Graphic::Render2D::UIMaterialCache in the
// context made those three fail to LINK on the one symbol the walk calls — the purity was real, and a
// concrete type would have ended it for a single function call.
//
// So the walk depends on the QUESTION and the backend supplies the ANSWER. Graphic::Render2D::UIMaterialCache
// is the only implementation; a suite that wants to test the walk's material path supplies a stub, which
// is the second thing this buys and the reason the batching rules can be asserted without a device.
namespace Desert::UI
{
    class IUIMaterialSource
    {
    public:
        virtual ~IUIMaterialSource() = default;

        // The opaque id the batcher records for @p handle, to be handed to DrawList2D::AddMaterialRect
        // and resolved back by the backend at Flush.
        //
        // NEVER NULL FOR A SET HANDLE. An implementation that cannot execute the material must answer
        // with something that DRAWS — the shipped one answers with a magenta error fill and names the
        // reason in the log — because an element that renders nothing is indistinguishable from an
        // element that was meant to render nothing. That is the failure mode UE shipped and never fixed
        // (`//TODO UMG Check if the material can be used with the UI`), and §1.4 forbids it here.
        [[nodiscard]] virtual const void* ResolveMaterial( const Common::AssetHandle& handle ) = 0;
    };
} // namespace Desert::UI

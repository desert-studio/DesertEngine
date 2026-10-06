#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/UI/UIMaterialSource.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Graphic
{
    class Framebuffer;
    class GraphicsPipeline;
} // namespace Desert::Graphic

namespace Desert::Graphic::Render2D
{
    // WHAT A UI MATERIAL IS, AND WHY IT IS THE `.demat` YOU ALREADY HAVE.
    //
    // Ю11 turns an element's look from a fixed effect list (gradient / glow / shadow / ring / glass) into
    // a surface an author extends. The surface is a `.demat` whose shader declares `Domain UI`: shader +
    // named parameter values, exactly what a material has been in this engine since the DSL existed.
    //
    // Nothing new was invented for the parameters. They travel down THE parameter transport
    // (Core/Formats/MaterialParamRow.hpp) — one row of the shared `Materials[]` storage buffer, named by
    // a push constant at offset 64 — which is why a UI material costs FOUR bytes of push constant beyond
    // the projection the batcher already pushed, and why sixteen author parameters cost the same four.
    // The alternative, per-element scalars in the push block, is what Slate does (three float4 in
    // FShaderParams) and it is also what puts those scalars IN Slate's batch key; glass has already shown
    // where that road ends, with all 128 bytes spent and no room for the next idea.
    //
    // ONE RUNTIME MATERIAL PER `.demat`, NOT PER ELEMENT. UE's UImage::GetDynamicMaterial() lazily
    // replaces a shared material with a per-widget instance the first time anybody animates a parameter,
    // and their own docs name that as the dominant real-world batching cost of UI materials. Here two
    // elements pointing at one asset share one entry, so they share one batch.
    class UIMaterialCache final : public ::Desert::UI::IUIMaterialSource
    {
    public:
        // One resolved material: the pipeline its shader is compiled into against the UI target, and the
        // runtime material holding its row and its samplers.
        struct Entry
        {
            std::unique_ptr<DataDrivenMaterial> Material;
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            // True for the shared error entry — the element draws the magenta hatch of `UIMatError`
            // because its slot named something the UI path cannot execute. Kept on the entry rather than
            // compared by pointer at the call site so the backend cannot forget to ask.
            bool     Error         = false;
            uint64_t LastUsedFrame = 0;
            // The binding layout Render2D declares this entry's draws against (RDG-FAULT1), kept with the pipeline
            // whose shader keys it. Mutable: the draw list hands the entry out const.
            mutable ShaderBindingLayoutCache Layout;
        };

        // (Re)build every pipeline against @p target. Called from Render2D::Init, i.e. after every
        // Scene::Init, because the framebuffers are recreated there and a pipeline outliving its render
        // pass is a device-lost, not a wrong picture. The materials themselves survive: they own
        // descriptor sets, not render passes.
        void Rebuild( const std::shared_ptr<Framebuffer>& target );

        // Resolve @p handle to the material an element fills itself with.
        //
        // NEVER NULL WHEN @p handle IS SET, and that is the point of this class. UE's answer to a
        // wrong-domain or not-yet-compiled material is to drop the batch: the widget renders silently
        // nothing, which is indistinguishable from a correctly invisible widget, and their own source
        // still carries `//TODO UMG Check if the material can be used with the UI`. Here every refusal
        // resolves to the error entry, is drawn as a magenta hatch, and is logged ONCE per handle with
        // the reason and both names.
        //
        // Returns nullptr only for an UNSET handle (the element has no material and draws its ordinary
        // fill) or when the UI target has not been built yet.
        [[nodiscard]] const Entry* Resolve( const Assets::AssetHandle& handle );

        // IUIMaterialSource. The walk holds this object through the interface and never through the
        // concrete type, so the opaque id it records is the Entry address Render2D::Flush casts back.
        [[nodiscard]] const void* ResolveMaterial( const Assets::AssetHandle& handle ) override
        {
            return Resolve( handle );
        }

        // Destroy the entries no frame still in flight can be reading. Called once per Flush, on the same
        // rule and with the same window as Render2D's texture executors — a UI material owns descriptor
        // sets exactly as they do.
        void RetireUnused();

        // Entries currently held. For the suite, so "one runtime material per asset, not per element" is
        // an assertion rather than a sentence in this comment.
        [[nodiscard]] std::size_t Size() const
        {
            return m_Entries.size();
        }

    private:
        // Build the pipeline + runtime material for @p shaderName, or nothing with @p refusal set.
        Entry Build( const std::string& shaderName, std::string& refusal ) const;

        // The magenta hatch, built on first need and shared by every failing handle.
        const Entry* ErrorEntry();

        std::shared_ptr<Framebuffer>                   m_Target;
        std::unordered_map<Assets::AssetHandle, Entry> m_Entries;
        std::unique_ptr<Entry>                         m_Error;
        // Handles already reported. A refusal at frame rate buries everything else in the log and gets
        // the whole message ignored; the picture is what says it is still wrong, every frame.
        std::unordered_map<Assets::AssetHandle, std::string> m_Reported;
    };
} // namespace Desert::Graphic::Render2D

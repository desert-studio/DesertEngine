#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Render2D/UIMaterialFallback.hpp>
#include <Engine/UI/UIMaterialSource.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
            // The material ASSET this entry was resolved from (MaterialService::AssetNameOf) - what a report
            // names, since many materials share one shader. Empty on the error entry.
            std::string AssetName;
            // The binding layout Render2D declares this entry's draws against (RDG-FAULT1), kept with the pipeline
            // whose shader keys it. Mutable: the draw list hands the entry out const.
            mutable ShaderBindingLayoutCache Layout;
            // The reload generation of Pipeline's shader when this entry was built (Shader::GetCodeGeneration):
            // a different one on Resolve rebuilds the entry (UIMaterialFallback::RebuildIfReloaded).
            uint64_t ShaderGeneration = 0;
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

        // The entry a draw of @p entry actually binds (RDG-FAULT1, UE's default-material fallback), PREPARED for
        // the draw (row, @p projection, row index written - PrepareDraw). An entry that cannot draw - its row does
        // not fit its shader's parameter layout, or the block Render2D declares for it fails the very
        // RDG::ValidatePassBindings the pass setup runs - would fault the WHOLE UI / present node; that draw binds
        // the error fill (the one default UI material) instead, logged once per material. Render2D::Resolve asks
        // this for every material draw, so the setup and the exec bind the same entry. Null when the entry has no
        // material, or when the error fill itself cannot draw (that draw is skipped in setup and exec alike).
        [[nodiscard]] const Entry* DrawableOrDefault( const Entry* entry, const glm::mat4& projection );

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

        // Build + the asset's authored values (overrides) + its name for @p handle, or nothing with @p refusal
        // set. What Resolve builds on a miss and what a hot shader reload rebuilds an entry with.
        Entry BuildFromAsset( const Assets::AssetHandle& handle, std::string& refusal ) const;

        // Rebuild @p entry in place when the shader its pipeline was built from has reloaded since
        // (UIMaterialFallback::RebuildIfReloaded); @p rebuild( std::string& refusal ) -> Entry builds the
        // replacement. The replaced pipeline and material are retired on the frames-in-flight window, never
        // destroyed under a frame that may still read them. The entry's address does not change.
        template <class Rebuild>
        void FollowShaderReload( Entry& entry, Rebuild&& rebuild );

        // The magenta hatch, built on first need and shared by every failing handle.
        const Entry* ErrorEntry();

        // Write what one draw of @p entry reads - its parameter row (only a row that fits, UIMaterialFallback::
        // RowFault), @p projection and row index 0 - then return why the draw cannot record, empty when it can:
        // the row's fault, else the error of RDG::ValidatePassBindings on exactly the block Render2D::DeclareInto
        // declares (the entry's kept layout + its executor's route fill). The ONE place a UI material draw is
        // prepared, so the fallback and the setup refusal cannot disagree.
        std::string PrepareDraw( const Entry& entry, const glm::mat4& projection );

        std::shared_ptr<Framebuffer>                   m_Target;
        std::unordered_map<Assets::AssetHandle, Entry> m_Entries;
        std::unique_ptr<Entry>                         m_Error;
        // What a hot-reload rebuild replaced, kept until no frame in flight can read it (RetireUnused).
        struct RetiredBuild
        {
            std::unique_ptr<DataDrivenMaterial> Material;
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            uint64_t                            Frame = 0;
        };
        std::vector<RetiredBuild> m_RetiredBuilds;
        // Handles already reported. A refusal at frame rate buries everything else in the log and gets
        // the whole message ignored; the picture is what says it is still wrong, every frame.
        std::unordered_map<Assets::AssetHandle, std::string> m_Reported;
        // Draws that fell back to the default for an unwritten row, reported once per material.
        UIMaterialFallback m_Fallback;
    };
} // namespace Desert::Graphic::Render2D

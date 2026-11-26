#pragma once

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>
#include <Engine/Graphic/View/TemporalAALayout.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace Desert::Graphic
{
    class ComputePipeline;

    // TAA1-B. The engine's own temporal resolve (UE: TemporalAA.usf), both methods in one class: TAA resolves at
    // RenderExtent (Native; Supersample, where SupersampleResolve follows it), TAAU reconstructs OutputExtent from
    // a smaller RenderExtent. One compute node "TemporalAA" (Programs/TemporalAA/TemporalAA.shader) per frame.
    //
    // TWO OUTPUTS OF ONE DISPATCH. The node writes the new history (HistoryRefs::Current, never touched again this
    // frame: next frame reprojects exactly what was resolved) and, at the same extent and with the same pixels,
    // the post chain's input — a graph transient "TAA.Output" returned in TemporalUpscalerOutputs::SceneColor. The
    // overlays drawn after the temporal pass (Debug, UI) draw into that transient; drawn into the history they
    // would be smeared into the following frames.
    //
    // TAA QUALITY is a shader variant (TAA_QUALITY=0/1/2, ShaderVariant defines): one pipeline per level, made on
    // first use; a quality change switches pipeline and keeps the history (ViewFrame.hpp, TemporalAAQuality).
    class TemporalAA final : public ITemporalUpscaler
    {
    public:
        // @p method TAA or TAAU; CreateTemporalUpscaler is the one caller and never passes None.
        explicit TemporalAA( TemporalMethod method );
        ~TemporalAA() override;

        [[nodiscard]] std::string_view DebugName() const override;
        [[nodiscard]] TemporalMethod   Method() const override
        {
            return m_Method;
        }

        [[nodiscard]] bool                            Supports( const ResolutionSplit& split ) const override;
        [[nodiscard]] std::vector<HistoryTextureDesc>
        HistoryDescs( const ResolutionSplit&                      split,
                      const Common::Scalability::ResolvedQuality& quality ) const override;
        [[nodiscard]] Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                   const TemporalUpscalerInputs& inputs ) const override;

        // The extent the resolve writes at @p split: Output for TAAU, Render for TAA (equal at Native; under
        // Supersample the downsample follows at Render).
        [[nodiscard]] RDG::Extent3D ResolveExtent( const ResolutionSplit& split ) const;

        // The permutation of quality @p quality (TAA_QUALITY=<quality>), cooked: ShippedByCook - the one statement
        // PipelineFor acquires and ProgramVariants lists.
        [[nodiscard]] static Core::ShaderVariant QualityVariant( uint32_t quality );
        // Every quality's permutation of "TemporalAA", cooked by the package (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

    private:
        // The pipeline of @p quality, created on first use (record time). Null with @p error set when the
        // variant does not compile or the pipeline is refused.
        [[nodiscard]] std::shared_ptr<ComputePipeline> PipelineFor( TemporalAAQuality quality,
                                                                    std::string&      error ) const;

        TemporalMethod                                          m_Method;
        mutable std::mutex                                      m_PipelineMutex;
        mutable std::array<std::shared_ptr<ComputePipeline>, 3> m_Pipelines{};
    };

    // The parameters of this frame's resolve (pure; the suite pins the values the shader relies on).
    [[nodiscard]] TemporalAAParams MakeTemporalAAParams( const ViewFrame& frame, RDG::Extent3D output );

    // The binding layout of TemporalAA.shader as the graph sees it — the C++ mirror of the shader's declarations.
    // The ShaderCacheKey suite pins it against the compiled shader's reflection (MakeShaderBindingLayout), and the
    // backend re-validates it at record time, so the two cannot drift silently.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TemporalAALayout();

    // The same for SupersampleResolve.shader (AddSupersampleResolve's two separable passes).
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& SupersampleResolveLayout();
} // namespace Desert::Graphic

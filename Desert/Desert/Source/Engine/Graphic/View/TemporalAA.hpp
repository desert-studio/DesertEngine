#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>

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
        [[nodiscard]] std::vector<HistoryTextureDesc> HistoryDescs( const ResolutionSplit& split ) const override;
        [[nodiscard]] Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                   const TemporalUpscalerInputs& inputs ) const override;

        // The extent the resolve writes at @p split: Output for TAAU, Render for TAA (equal at Native; under
        // Supersample the downsample follows at Render).
        [[nodiscard]] RDG::Extent3D ResolveExtent( const ResolutionSplit& split ) const;

    private:
        // The pipeline of @p quality, created on first use (record time). Null with @p error set when the
        // variant does not compile or the pipeline is refused.
        [[nodiscard]] std::shared_ptr<ComputePipeline> PipelineFor( TemporalAAQuality quality,
                                                                    std::string&      error ) const;

        TemporalMethod                                          m_Method;
        mutable std::mutex                                      m_PipelineMutex;
        mutable std::array<std::shared_ptr<ComputePipeline>, 3> m_Pipelines{};
    };

    // The resolve's parameters, the C++ side of TemporalAA.shader's `TemporalAABuffer` (std430). A buffer and not
    // a push constant: two matrices are already the 128 bytes Vulkan guarantees for push constants.
    struct TemporalAAParams
    {
        glm::mat4 InvViewProjection;   // this frame, unjittered: output pixel uv (+ device depth) -> world
        glm::mat4 PrevViewProjection;  // previous frame, unjittered: world -> previous clip
        glm::vec2 RenderSize;          // pixels
        glm::vec2 OutputSize;          // pixels
        glm::vec2 JitterUv;            // this frame's jitter in uv: JitterNdc * (0.5, -0.5)
        float     HistoryValid = 0.0f; // 1 when the history holds this view's previous frame
        float     Pad          = 0.0f;
    };
    static_assert( offsetof( TemporalAAParams, PrevViewProjection ) == 64 );
    static_assert( offsetof( TemporalAAParams, RenderSize ) == 128 );
    static_assert( offsetof( TemporalAAParams, OutputSize ) == 136 );
    static_assert( offsetof( TemporalAAParams, JitterUv ) == 144 );
    static_assert( offsetof( TemporalAAParams, HistoryValid ) == 152 );
    static_assert( sizeof( TemporalAAParams ) == 160 );

    // The parameters of this frame's resolve (pure; the suite pins the values the shader relies on).
    [[nodiscard]] TemporalAAParams MakeTemporalAAParams( const ViewFrame& frame, RDG::Extent3D output );

    // The binding layout of TemporalAA.shader as the graph sees it — the C++ mirror of the shader's declarations.
    // The ShaderCacheKey suite pins it against the compiled shader's reflection (MakeShaderBindingLayout), and the
    // backend re-validates it at record time, so the two cannot drift silently.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TemporalAALayout();

    // The same for SupersampleResolve.shader (AddSupersampleResolve's two separable passes).
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& SupersampleResolveLayout();

    // SupersampleResolve.shader's push constants: one axis of the separable Catmull-Rom.
    struct SupersampleResolvePush
    {
        int32_t SourceWidth       = 0;
        int32_t SourceHeight      = 0;
        int32_t DestinationWidth  = 0;
        int32_t DestinationHeight = 0;
        int32_t Axis              = 0; // 0: horizontal, 1: vertical
        int32_t Pad[3]            = {};
    };
    static_assert( sizeof( SupersampleResolvePush ) == 32 );
} // namespace Desert::Graphic

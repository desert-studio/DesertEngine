#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// THE RESTIR GI FINAL GATHER (Ouyang et al. 2021, "ReSTIR GI: Path Resampling for Real-Time Path Tracing"; UE 5.4+
// r.Lumen.ScreenProbeGather.ReSTIR / LumenReSTIRGather.usf; NVIDIA RTXDI ReSTIR GI). The screen probes' final
// gather's alternative on the hardware tracer (ScreenProbeGatherInputs::ReSTIR, ReSTIRGatherRuns): instead of
// probes on a grid and their filtered radiance, one ray a reservoir texel (the G-buffer's extent /
// kReSTIRDownsample), its sample streamed through reservoirs:
//   ReSTIRGI: Initial   the reservoir's pixel this frame traces one cosine ray (GITraceRay, lit by GIHitRadiance)
//   ReSTIRGI: Temporal  + the view's previous reservoir of the same surface (reprojected; history in
//                       ScreenProbeGatherHistory::Reservoir, the gather's existing per-view history)
//   ReSTIRGI: Spatial   + kReSTIRSpatialSamples neighbours (Jacobian-corrected, visibility-tested)
//   ReSTIRGI: Resolve   per pixel into the integrate's three textures (diffuse, rough specular, backface)
// Then the screen probes' own disocclusion and temporal filter take them (ScreenProbeGather::AddPasses): one
// denoiser, one history of the filtered lighting, one ScreenProbeGatherFrame for the composite.
// Shaders: Programs/GlobalIllumination/ReSTIRGI*.shader; the shared math: Common/ReSTIRGI.glslh.
namespace Desert::Graphic
{
    class Shader;
    class ComputePipeline;
    class ScreenProbeGatherHistory; // Graphic/View/SceneViewState.hpp
} // namespace Desert::Graphic

namespace Desert::Graphic::GlobalIllumination
{
    struct ScreenProbeGatherInputs; // ScreenProbeGather.hpp

    // UE r.Lumen.ReSTIRGather defaults: the reservoirs at half the G-buffer's resolution (must match
    // Common/ReSTIRGI.glslh RESTIR_DOWNSAMPLE), four spatial neighbours, a temporal history capped at ten samples
    // (a changed light replaces a stale sample within that many frames).
    inline constexpr uint32_t kReSTIRDownsample      = 2;
    inline constexpr uint32_t kReSTIRSpatialSamples  = 4;
    inline constexpr float    kReSTIRTemporalMaxM    = 10.0f;
    inline constexpr float    kReSTIRSpatialRadius   = 16.0f; // reservoir texels
    inline constexpr float    kReSTIRDepthTolerance  = 0.1f;  // relative distance to the camera
    inline constexpr float    kReSTIRNormalTolerance = 0.9f;  // cosine

    // ReSTIRParams.extent.w — must match Common/ReSTIRGI.glslh RESTIR_FLAG_*.
    inline constexpr uint32_t kReSTIRFlagPreviousValid = 1u;

    // std430 — must match `struct ReSTIRParams` in Common/ReSTIRGI.glslh. 80 bytes.
    struct ReSTIRParamsPayload
    {
        glm::uvec4 Extent{ 0u };     // xy = reservoir extent, z = spatial samples, w = kReSTIRFlag*
        glm::uvec4 Frame{ 0u };      // x = frame index
        glm::vec4  Tuning{ 0.0f };   // x = temporal M cap, y = spatial radius, z = depth tolerance, w = normal cos
        glm::vec4  Jitter{ 0.0f };   // xy = this frame's NDC jitter, zw = last frame's
        glm::vec4  RayClamp{ 0.0f }; // x = the largest radiance a ray carries (absolute)
    };
    static_assert( sizeof( ReSTIRParamsPayload ) == 80 );

    // The reservoir extent of a G-buffer extent (rounded up).
    [[nodiscard]] constexpr glm::uvec2 ReSTIRReservoirExtent( const glm::uvec2 extent )
    {
        return ( extent + glm::uvec2( kReSTIRDownsample - 1u ) ) / kReSTIRDownsample;
    }

    // The three textures the resolve writes (the screen probes' integrate's: ScreenProbeGather::AddPasses creates
    // them and hands them to the disocclusion and the temporal filter after).
    struct ReSTIRGatherOutputs
    {
        RDG::TextureRef Diffuse;
        RDG::TextureRef Specular;
        RDG::TextureRef Backface;
    };

    class ReSTIRGather
    {
    public:
        ReSTIRGather();
        ~ReSTIRGather();
        ReSTIRGather( const ReSTIRGather& )            = delete;
        ReSTIRGather& operator=( const ReSTIRGather& ) = delete;

        // The four programs: Initial and Spatial under TracerVariant( @p pipeline ) (re-acquired when it changes),
        // Temporal and Resolve once. A refusal names the program.
        [[nodiscard]] Common::BoolResultStr EnsurePrograms( const Pipeline& pipeline );

        // Every program variant EnsurePrograms can acquire: the tracing two under the hardware entries of
        // kTracerPermutations (ReSTIRGatherRuns: the gather runs on no other tracer), the other two once. Each is
        // ShaderVariant::ShippedByCook.
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

        // The gather of one frame into @p outputs. @p rayClamp = the screen probes' absolute ray clamp
        // (ScreenProbeParamsPayload::RayClamp.x); @p historyReadable = the view's previous frame is a previous
        // frame of this one (ViewFrame::HistoryValid; the reservoirs reuse with or without the denoiser). The caller
        // has called inputs.FinishRadianceCache (the initial sample and the resolve read the cache's probes).
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, const ScreenProbeGatherInputs& inputs,
                                                       float rayClamp, bool historyReadable,
                                                       ScreenProbeGatherHistory&  history,
                                                       const ReSTIRGatherOutputs& outputs );

    private:
        enum Program : uint32_t
        {
            Initial = 0,
            Temporal,
            Spatial,
            Resolve,
            ProgramCount
        };

        [[nodiscard]] Common::BoolResultStr Acquire( Program program, const Core::ShaderVariant& variant );

        std::array<std::shared_ptr<Shader>, ProgramCount>          m_Shaders;
        std::array<std::shared_ptr<ComputePipeline>, ProgramCount> m_Pipelines;
        mutable std::array<ShaderBindingLayoutCache, ProgramCount> m_Layouts;
        Pipeline                                                   m_TracingProgramsFor{};
        bool                                                       m_TracingProgramsValid = false;
        uint32_t                                                   m_FrameIndex           = 0;
        bool                                                       m_Logged               = false;
    };
} // namespace Desert::Graphic::GlobalIllumination

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
#include <string_view>
#include <vector>

// THE RESTIR GI FINAL GATHER (Ouyang et al. 2021, "ReSTIR GI: Path Resampling for Real-Time Path Tracing"; UE 5.4+
// r.Lumen.ScreenProbeGather.ReSTIR / LumenReSTIRGather.usf; NVIDIA RTXDI ReSTIR GI). The screen probes' final
// gather's alternative on the hardware tracer (ScreenProbeGatherInputs::ReSTIR, ReSTIRGatherRuns): instead of
// probes on a grid and their filtered radiance, one ray a reservoir texel (the G-buffer's extent /
// kReSTIRDownsample), its sample streamed through reservoirs:
//   ReSTIRGI: Initial   the reservoir's pixel this frame traces one cosine ray (GITraceRay, lit by GIHitRadiance);
//                       half the texels (a checkerboard flipped per frame) also re-trace the history sample the
//                       temporal pass would reuse and drop it when its hit or its light changed (validation rays)
//   ReSTIRGI: Temporal  + the view's previous reservoir of the same surface (reprojected; history in
//                       ScreenProbeGatherHistory::Reservoir, the gather's existing per-view history); not added on
//                       a frame without one - the initial pass (InitialNoHistory) then writes the history itself
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

    // THE HISTORY'S VALIDATION RAYS (UE's ReSTIR gather validation; ReSTIRGIInitial.shader ValidateHistory): every
    // frame a checkerboard half of the reservoir texels re-traces the history sample the temporal pass would reuse
    // and drops it (M and W start over) when the hit moved by more than kReSTIRValidationDistance relative or its
    // luminance changed by more than kReSTIRValidationLuminance relative - so a light switched or moved, an
    // occluder added or removed, reaches the indirect light within two frames instead of kReSTIRTemporalMaxM.
    // A luminance under kReSTIRValidationFloor x the ray clamp is noise, not a light.
    inline constexpr float kReSTIRValidationLuminance = 0.25f;
    inline constexpr float kReSTIRValidationDistance  = 0.05f;
    inline constexpr float kReSTIRValidationFloor     = 1.0e-4f;

    // The initial pass's permutation for a frame without the view's previous reservoirs (UE: temporal resampling
    // skipped when !bIsHistoryValid): no validation rays, no previous texture declared, and its reservoirs ARE the
    // view's reservoir history - the temporal pass is not added (ReSTIRGather::AddPasses). ReSTIRGIInitial.shader.
    inline constexpr std::string_view kReSTIRInitialNoHistoryDefine = "RESTIR_INITIAL_NO_HISTORY";

    // std430 — must match `struct ReSTIRParams` in Common/ReSTIRGI.glslh. 96 bytes.
    struct ReSTIRParamsPayload
    {
        glm::uvec4 Extent{ 0u };     // xy = reservoir extent, z = spatial samples, w = 0 (unused)
        glm::uvec4 Frame{ 0u };      // x = frame index
        glm::vec4  Tuning{ 0.0f };   // x = temporal M cap, y = spatial radius, z = depth tolerance, w = normal cos
        glm::vec4  Jitter{ 0.0f };   // xy = this frame's NDC jitter, zw = last frame's
        glm::vec4  RayClamp{ 0.0f }; // x = the largest radiance a ray carries (absolute)
        glm::vec4  Validation{ 0.0f }; // x = luminance change, y = hit distance change (relative), z = floor (abs)
    };
    static_assert( sizeof( ReSTIRParamsPayload ) == 96 );

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

        // The five programs: Initial, InitialNoHistory (+ kReSTIRInitialNoHistoryDefine) and Spatial under
        // TracerVariant( @p pipeline ) (re-acquired when it changes), Temporal and Resolve once. A refusal names
        // the program.
        [[nodiscard]] Common::BoolResultStr EnsurePrograms( const Pipeline& pipeline );

        // Every program variant EnsurePrograms can acquire: the tracing three under the hardware entries of
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
            InitialNoHistory, // ReSTIRGIInitial + kReSTIRInitialNoHistoryDefine: a frame without previous
                              // reservoirs
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
        bool                                                       m_LoggedNoHistory      = false;
    };
} // namespace Desert::Graphic::GlobalIllumination

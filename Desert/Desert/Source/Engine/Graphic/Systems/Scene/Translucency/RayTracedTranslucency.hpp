#pragma once

// NG-RT-REFRACTION3A contract (design F:/aftmp/NG-RT-REFRACTION3A-DESIGN.md). Owned by nobody while the packages
// run.
//
// RAY-TRACED TRANSLUCENCY (UE RayTracedTranslucency: refraction rays from the translucent front layer, lit at the
// hit): one graph pass, "Translucency: Ray Traced Refraction", at HALF the render resolution: one ray per 2x2 quad
// of the front layer G-buffer (kRayTracedTranslucencyQuad). The ray generation picks the quad's pixel by
// FrameIndex & 3 (index = x + 2 * y inside the quad), or the quad's first pixel that holds a front layer when
// that one does not; a quad without one writes zero and traces nothing. The ray lets the surface hit groups
// answer each dielectric interface (DesertRayPayload Transmission / Ior), follows Snell through at most
// `MaxInterfaces` of them and lights the first opaque hit SCREEN FIRST: a hit the view sees (its projection by
// giFrame.viewProjection lies in the frame and the G-buffer depth there is the hit within
// GIRayBias().screenTolerance, the test Common/GIHitLighting.glslh GIHitMaterial makes) takes this frame's lit
// scene colour from the scene copy; a hit the view does not see is lit as the reflections of the tier light
// theirs (RayTracedTranslucencyTracer of Reflections.HitLighting): from the surface cache under 0, in place under
// 1 (GIHitRadianceLit). The glass cell (Mesh/Surface/Pass_Forward_Translucent.glslh through
// Mesh/Surface/TranslucencyRefraction.glslh) reads the half-size result where its fragment is the front layer;
// where no quad around it answered for its layer the cell reads the scene copy itself (the ScreenSpace tier).
// A hit lit in place reads the world radiance cache off screen, where no G-buffer pixel marked its cells: the pass
// WRITES a mark request for it into the cache's request table (RadianceCacheFrame::MarkRequests,
// GlobalIllumination/RadianceCacheMarkRequests.hpp), which the next frame's RadianceCacheMark stamps and clears.
//
// The pass is a ray tracing pipeline over THIS FRAME's hit group table under either tracer (the hit groups answer
// each interface), as "Deferred: Reflections (HitLighting)" is (SSRRenderer::EnsureHitLightingPipeline /
// RecordHitLighting): the pipeline's hit groups are the table's Templates, so it is ensured per frame, never at
// start-up, and everything the hit groups and the hit lighting (in place or from the surface cache) read is the
// reflections' own record, SSRRenderer::TracedReads - no field of it is repeated here.
//
// Package A (the frame, CPU side) produces RayTracedTranslucencyInputs and the push and calls AddPass.
// Package B (the pass and its shaders) implements the class (RayTracedTranslucency.cpp), the ray generation
// program and the glass cell's read. MeshRenderer's binding of `RefractionOutputs::Radiance` into the glass draw
// by kTranslucencyRefractionTexture is unchanged and belongs to neither.
//
// THE PUSH BLOCK is declared once, in the shader header Common/RayTracedTranslucencyPush.glslh (contract file):
// DesertHeaderTool generates ShaderPush::RayTracedTranslucencyPush from it on every build
// (Engine/Generated/ShaderPushConstants.gen.hpp), the program includes it. There is no hand-written C++ twin.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/RadianceCacheMarkRequests.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneReads.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SSRRenderer.hpp> // TracedReads, FrontLayerInputs, the SSR*ShaderParameters

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Graphic::System
{
    // The program: `Shader "RayTracedTranslucency"` (Programs/Translucency/RayTracedTranslucency.shader), its one
    // pass `Pass "Trace"` with a RayGen and a Miss stage. kRayTracedTranslucencyProgram is what
    // Runtime::AcquireShaderVariant takes (as "SSR/HitLighting").
    inline constexpr std::string_view kRayTracedTranslucencyShader  = "RayTracedTranslucency";
    inline constexpr std::string_view kRayTracedTranslucencyProgram = "RayTracedTranslucency/Trace";
    // The graph pass's name (profile-dump, the acceptance's "did execute" line).
    inline constexpr std::string_view kRayTracedTranslucencyPass = "Translucency: Ray Traced Refraction";
    // The glass cell's texture (bound by name, the number is the program build's): rgb = the pre-exposed radiance
    // seen through the surface, already multiplied by the path's transmittance. HALF size: texel q answers for
    // the full-resolution quad [2q, 2q + 1]. a IS A CODE, never filtered (read by texelFetch): 0 = no answer for
    // this quad (no front layer in it, too rough, the interface budget exhausted, or Composite 0); 1 + i
    // (i = 0..3, exact in rgba16f) = answered, and the ray started at the quad's pixel 2q + ( i & 1, i >> 1 ).
    // The cell's function (DesertTranslucencyRefraction) turns it into the 0..1 weight its caller mixes by. The
    // graph's transparent black when the view does not trace: a = 0 everywhere.
    inline constexpr std::string_view kTranslucencyRefractionTexture = "TranslucencyRefractionTexture";
    // The output's format is the scene colour's (ViewTargetFormats: linear HDR, pre-exposed).
    // The quad's side: launch and output are ceil( render resolution / kRayTracedTranslucencyQuad ) per axis.
    inline constexpr uint32_t kRayTracedTranslucencyQuad = 2;
    [[nodiscard]] constexpr uint32_t RayTracedTranslucencyLaunchSize( const uint32_t full )
    {
        return ( full + kRayTracedTranslucencyQuad - 1 ) / kRayTracedTranslucencyQuad;
    }

    // Translucency.Refraction of Common::Scalability: what answers the refraction. The row's value names are
    // Common::Scalability::kTranslucencyRefractionMethodNames, index = this enum's value. THE ORDER CHECK'S OWNER is
    // RayTracedTranslucency.cpp (package A): static_assert that the array's size is 2 and that
    // names[ScreenSpace] == "ScreenSpace" and names[HardwareTraced] == "HardwareTraced" (constexpr string_view ==).
    enum class TranslucencyRefractionMethod : uint8_t
    {
        ScreenSpace    = 0, // the scene copy bent on screen (every tier without a TLAS, and the Forward path)
        HardwareTraced = 1, // this pass
    };

    // The tracer the program is compiled under and the reads are made for, a function of the quality row and the
    // device alone (@p inPlace = SceneRendererImpl::HitLightingInPlace(): Reflections.HitLighting is on and the
    // device offers the ray tracing pipeline), whatever Reflections.Method is this frame: the hardware tracer,
    // hits lit in place (GlobalIllumination::kTracerPermutations[2]) or from the surface cache's cards
    // (kTracerPermutations[1]) - the rule the hardware-traced reflections follow (UE Lumen: hit lighting only on
    // the top tier).
    [[nodiscard]] constexpr GlobalIllumination::Pipeline RayTracedTranslucencyTracer( const bool inPlace )
    {
        return GlobalIllumination::Pipeline{ .Traced = true,
                                             .Tracer = GlobalIllumination::Tracer::HardwareRayQuery,
                                             .Hits   = inPlace ? GlobalIllumination::HitLighting::InPlace
                                                               : GlobalIllumination::HitLighting::SurfaceCache };
    }

    // The frame's defaults of ShaderPush::RayTracedTranslucencyPush (the block has no defaults of its own).
    // MaxInterfaces and Composite have none here: they are the tier's rows
    // (Common::Scalability::Parameter::TranslucencyRefractionMaxInterfaces / TranslucencyRefractionComposite).
    inline constexpr float kRayTracedTranslucencyMaxRayDistance = 30000.0f; // cm, each segment
    inline constexpr float kRayTracedTranslucencyMaxRoughness   = 0.4f;     // above it the surface is not traced

    // THE SLOT NAMES, spelled once for the C++ bindings (A) and the program's declarations (B). The whole set the
    // ray generation declares BY THESE NAMES (a ray tracing pipeline's layout is its ray generation's):
    //   1. this struct: the output image, the view block the hit groups read, the hit groups' scene reads
    //      (Common/CameraUB.glslh, Common/RayTracingSceneReads.glslh);
    //   2. SSRFrontLayerShaderParameters: FrontLayerNormalTexture (rgb normal, a roughness),
    //      FrontLayerDepthTexture, PointClampSampler;
    //   3. SSRShaderParameters as SSRRenderer::DeclareTracedReads binds them for the pass's tracer
    //      (RayTracedTranslucencyTracer: GIFrameUB, PointLightsUB, SpotLightsUB, LightGridParamsUB,
    //      LightGridWorldCellsUB, RadianceCache*, EnvironmentSpecularTexture, LinearClampSampler, GBufferATexture,
    //      GBufferEmissiveTexture, GBufferShadingWordTexture, PointClampSampler), the tracer's own reads
    //      (GlobalIllumination::DeclareTracerReads: the TLAS and its metadata, Common/GITracer.glslh) and, for the
    //      surface cache tracer, the cards' (GlobalIllumination::DeclareSurfaceCacheReads).
    // Binding NUMBERS are the program's; nothing on the C++ side names one.
    struct RayTracedTranslucencyShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, RefractionOutputTexture ); // rgba16f, half size, one texel per quad
        // This frame's scene snapshot (the opaque lit scene, pre-exposed): the radiance of a hit the view sees.
        // Sampled through LinearClampSampler at the hit's screen uv.
        DESERT_SHADER_PARAMETER( Texture, SceneColorCopyTexture );
        DESERT_SHADER_PARAMETER( UniformBuffer, CameraUB );
        // The OPAQUE scene depth: Common/GIHitLighting.glslh (GIHitMaterial) checks a hit against the screen with it.
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        // The radiance cache's mark request table (HitLighting.RadianceCache.MarkRequests), WRITTEN: one request
        // per path end lit in place. Named by the contract (GlobalIllumination::kMarkRequestsBinding).
        static constexpr RDG::ShaderParameterOf<RDG::ShaderResourceKind::StorageBuffer> RadianceCacheMarkRequestsUB{
             GlobalIllumination::kMarkRequestsBinding };
        DESERT_RAY_TRACING_SCENE_READ_PARAMETERS;
    };

    // What the pass reads. Nothing here repeats a field of the structs it names.
    struct RayTracedTranslucencyInputs
    {
        // The FRONT LAYER's G-buffer ("Translucency: Front Layer G-buffer"): Normal (a = roughness) and Depth.
        SSRRenderer::FrontLayerInputs FrontLayer;
        // CameraUB of this view and frame (ShaderProtocols::MakeCameraUB, uploaded by the caller).
        RDG::BufferRef Camera;
        // The opaque G-buffer depth of this view (not the front layer's), bound as GBufferDepthTexture.
        RDG::TextureRef OpaqueDepth;
        // The hit groups' and the hit lighting's reads, the reflections' record made for the tracer EnsurePipeline
        // was given (RayTracedTranslucencyTracer): Tracer (the TLAS and metadata), HitGroups (NON-NULL under
        // either tracer - the segments' Transmission / Ior come from the hit groups: this frame's table, owned by
        // the SceneRenderer for the frame, its Templates are what EnsurePipeline was given), SceneReads (valid),
        // GIFrame, lights, world light grid, radiance cache, EnvSpecular, G-buffer A / emissive / shading word,
        // and SurfaceCache (complete when the tracer's Hits is SurfaceCache, empty under InPlace). ShadowCascades
        // and Shadow stay empty (TracedReadsCascades is false for the hardware tracer).
        SSRRenderer::TracedReads HitLighting;
        // This frame's scene snapshot (FrameTransients::SceneColorCopy, the texture the glass node reads), bound
        // as SceneColorCopyTexture: a declared read of the pass (RDG::Access::SampledCompute).
        RDG::TextureRef SceneCopy;
        // The render resolution: the front layer's and the opaque G-buffer's size.
        uint32_t FullWidth  = 0;
        uint32_t FullHeight = 0;
        // The launch size and the output's size: RayTracedTranslucencyLaunchSize( FullWidth / FullHeight ).
        // AddPass refuses any other pair.
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };

    struct RefractionOutputs
    {
        RDG::TextureRef Radiance; // bound into the glass draw as kTranslucencyRefractionTexture
    };

    // One per view (a member of the view's renderer, as RayTracedSunShadowRenderer). No state across frames beside
    // the pipeline and the template list it was built for.
    class RayTracedTranslucencyRenderer
    {
    public:
        RayTracedTranslucencyRenderer();
        ~RayTracedTranslucencyRenderer();
        RayTracedTranslucencyRenderer( const RayTracedTranslucencyRenderer& )            = delete;
        RayTracedTranslucencyRenderer& operator=( const RayTracedTranslucencyRenderer& ) = delete;

        // The program's permutation: GlobalIllumination::TracerVariant( @p tracer ) (cooked), plus
        // kRayTracingReorderDefine when @p reorder (RayTracingReorderOffered(), as SSRRenderer::
        // HitLightingVariant). The one statement EnsurePipeline acquires and ProgramVariants lists.
        [[nodiscard]] static Core::ShaderVariant Variant( const GlobalIllumination::Pipeline& tracer,
                                                          bool                                reorder );
        // Every permutation of kRayTracedTranslucencyShader (RayTracedTranslucencyTracer of either row value x
        // reorder: 4), for the package cook (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

        // GRAPH BUILD, every frame the pass runs, before AddPass: the ray tracing pipeline of
        // kRayTracedTranslucencyProgram under @p tracer (RayTracedTranslucencyTracer of this frame's row) with one
        // hit group per surface template of this frame's hit group table (@p templates =
        // RayTracingHitGroupTable::Templates, each "<Template>/RayTracing.Hit"). Rebuilt only when the template
        // list or the permutation changes; the tracer it was built for is what AddPass checks the reads against.
        // The error names what was refused (no ray tracing pipeline on the device, the program or a hit cell did
        // not compile); the caller logs it once and the view stays ScreenSpace.
        [[nodiscard]] Common::BoolResultStr EnsurePipeline( const GlobalIllumination::Pipeline& tracer,
                                                            const std::vector<std::string>&     templates );

        // Adds the pass to @p graph: declares the bindings named above, creates the output (in.Width x in.Height),
        // and records one ray generation per quad (Renderer::TraceRays with @p in.HitLighting.HitGroups). It is
        // added whatever push.Composite says (0 changes the answer's a, never the rays). The pass captures the
        // pipeline EnsurePipeline published at this graph build (a shared_ptr), never the member a later frame may
        // republish. The error is the frame's reason for having no traced refraction: EnsurePipeline did not
        // succeed for this table's templates, the reads were made for another tracer than the pipeline's, or an
        // input is missing (it names which; the surface cache's reads under the surface cache tracer).
        [[nodiscard]] Common::ResultStr<RefractionOutputs> AddPass( RDG::Builder&                             graph,
                                                                    const RayTracedTranslucencyInputs&        in,
                                                                    const ShaderPush::RayTracedTranslucencyPush& push );

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Graphic::System

#pragma once

// NG-RT-REFRACTION contract (design F:/aftmp/NG-RT-REFRACTION-DESIGN.md). Owned by nobody while the packages run.
//
// RAY-TRACED TRANSLUCENCY (UE RayTracedTranslucency: refraction rays from the translucent front layer, lit at the
// hit): one graph pass, "Translucency: Ray Traced Refraction", that starts a ray at every pixel of the front layer
// G-buffer, lets the surface hit groups answer each dielectric interface (DesertRayPayload Transmission / Ior),
// follows Snell through at most `MaxInterfaces` of them, lights the first opaque hit as the hit-lit reflections do
// (Common/GIHitLighting.glslh GIHitRadianceLit) and writes what the glass sees THROUGH itself. The glass cell
// (Mesh/Surface/Pass_Forward_Translucent.glslh) reads the result where its fragment is the front layer; where the
// trace did not answer (a = 0) the cell reads the scene copy itself - this pass never reads the scene copy.
//
// The pass is a ray tracing pipeline over THIS FRAME's hit group table, exactly as "Deferred: Reflections
// (HitLighting)" is (SSRRenderer::EnsureHitLightingPipeline / RecordHitLighting): the pipeline's hit groups are the
// table's Templates, so it is ensured per frame, never at start-up, and everything the hit groups and the in-place
// hit lighting read is the reflections' own record, SSRRenderer::TracedReads - no field of it is repeated here.
//
// Package A implements the class (RayTracedTranslucency.cpp), produces the reads and calls it from the frame.
// Package B writes the ray generation program and the glass cell's read.
// Package C binds `RefractionOutputs::Radiance` into the glass draw (MeshRenderer) by kTranslucencyRefractionTexture.
//
// THE PUSH BLOCK is declared once, in the shader header Common/RayTracedTranslucencyPush.glslh (contract file):
// DesertHeaderTool generates ShaderPush::RayTracedTranslucencyPush from it on every build
// (Engine/Generated/ShaderPushConstants.gen.hpp), the program includes it. There is no hand-written C++ twin.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
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
    // seen through the surface, already multiplied by the path's transmittance; a = 1 where the trace answered,
    // 0 where it did not (the cell then reads the scene copy, the ScreenSpace tier's statement).
    inline constexpr std::string_view kTranslucencyRefractionTexture = "TranslucencyRefractionTexture";
    // The output's format is the scene colour's (ViewTargetFormats: linear HDR, pre-exposed).

    // Translucency.Refraction of Common::Scalability: what answers the refraction. The row's value names are
    // Common::Scalability::kTranslucencyRefractionMethodNames, index = this enum's value. THE ORDER CHECK'S OWNER is
    // RayTracedTranslucency.cpp (package A): static_assert that the array's size is 2 and that
    // names[ScreenSpace] == "ScreenSpace" and names[HardwareTraced] == "HardwareTraced" (constexpr string_view ==).
    enum class TranslucencyRefractionMethod : uint8_t
    {
        ScreenSpace    = 0, // the scene copy bent on screen (every tier without a TLAS, and the Forward path)
        HardwareTraced = 1, // this pass
    };

    // The tracer the program is compiled under and the reads are made for: the hardware tracer, hits lit in place
    // (GlobalIllumination::kTracerPermutations[2]) - whatever Reflections.Method is this frame.
    inline constexpr GlobalIllumination::Pipeline kRayTracedTranslucencyTracer{
         .Traced = true,
         .Tracer = GlobalIllumination::Tracer::HardwareRayQuery,
         .Hits   = GlobalIllumination::HitLighting::InPlace };

    // The frame's defaults of ShaderPush::RayTracedTranslucencyPush (the block has no defaults of its own).
    inline constexpr float    kRayTracedTranslucencyMaxRayDistance = 30000.0f; // cm, each segment
    inline constexpr float    kRayTracedTranslucencyMaxRoughness   = 0.4f;     // above it the surface is not traced
    inline constexpr uint32_t kRayTracedTranslucencyMaxInterfaces  = 4;        // UE MaxPrimaryHitEvents

    // THE SLOT NAMES, spelled once for the C++ bindings (A) and the program's declarations (B). The whole set the
    // ray generation declares BY THESE NAMES (a ray tracing pipeline's layout is its ray generation's):
    //   1. this struct: the output image, the view block the hit groups read, the hit groups' scene reads
    //      (Common/CameraUB.glslh, Common/RayTracingSceneReads.glslh);
    //   2. SSRFrontLayerShaderParameters: FrontLayerNormalTexture (rgb normal, a roughness), FrontLayerDepthTexture,
    //      PointClampSampler;
    //   3. SSRShaderParameters as SSRRenderer::DeclareTracedReads binds them for kRayTracedTranslucencyTracer
    //      (GIFrameUB, PointLightsUB, SpotLightsUB, LightGridParamsUB, LightGridWorldCellsUB, RadianceCache*,
    //      EnvironmentSpecularTexture, LinearClampSampler, GBufferATexture, GBufferEmissiveTexture,
    //      GBufferShadingWordTexture, PointClampSampler) and the tracer's own reads
    //      (GlobalIllumination::DeclareTracerReads: the TLAS and its metadata, Common/GITracer.glslh).
    // Binding NUMBERS are the program's; nothing on the C++ side names one.
    struct RayTracedTranslucencyShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, RefractionOutputTexture ); // rgba16f, written per front-layer pixel
        DESERT_SHADER_PARAMETER( UniformBuffer, CameraUB );
        // The OPAQUE scene depth: Common/GIHitLighting.glslh (GIHitMaterial) checks a hit against the screen with it.
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
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
        // The hit groups' and the in-place hit lighting's reads, the reflections' record made for
        // kRayTracedTranslucencyTracer: Tracer (the TLAS and metadata), HitGroups (NON-NULL: this frame's table,
        // owned by the SceneRenderer for the frame, its Templates are what EnsurePipeline was given), SceneReads
        // (valid), GIFrame, lights, world light grid, radiance cache, EnvSpecular, G-buffer A / emissive / shading
        // word. SurfaceCache, ShadowCascades and Shadow stay empty (TracedReadsCascades is false for this tracer).
        SSRRenderer::TracedReads HitLighting;
        // The render resolution: the output's size and the launch size.
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

        // The program's permutation: GlobalIllumination::TracerVariant( kRayTracedTranslucencyTracer ) (cooked),
        // plus kRayTracingReorderDefine when @p reorder (RayTracingReorderOffered(), as SSRRenderer::
        // HitLightingVariant). The one statement EnsurePipeline acquires and ProgramVariants lists.
        [[nodiscard]] static Core::ShaderVariant Variant( bool reorder );
        // Both permutations of kRayTracedTranslucencyShader, for the package cook (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

        // GRAPH BUILD, every frame the pass runs, before AddPass: the ray tracing pipeline of
        // kRayTracedTranslucencyProgram with one hit group per surface template of this frame's hit group table
        // (@p templates = RayTracingHitGroupTable::Templates, each "<Template>/RayTracing.Hit"). Rebuilt only when
        // the template list or the permutation changes. The error names what was refused (no ray tracing pipeline
        // on the device, the program or a hit cell did not compile); the caller logs it once and the view stays
        // ScreenSpace.
        [[nodiscard]] Common::BoolResultStr EnsurePipeline( const std::vector<std::string>& templates );

        // Adds the pass to @p graph: declares the bindings named above, creates the output, and records one ray
        // generation per pixel (Renderer::TraceRays with @p in.HitLighting.HitGroups). The pass captures the
        // pipeline EnsurePipeline published at this graph build (a shared_ptr), never the member a later frame may
        // republish. The error is the frame's reason for having no traced refraction: EnsurePipeline did not
        // succeed for this table's templates, or an input is missing (it names which).
        [[nodiscard]] Common::ResultStr<RefractionOutputs> AddPass( RDG::Builder&                             graph,
                                                                    const RayTracedTranslucencyInputs&        in,
                                                                    const ShaderPush::RayTracedTranslucencyPush& push );

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Graphic::System

#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/ScreenSpaceDenoiser.hpp>
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>

// MEGALIGHTS (UE 5.5 Renderer/Private/MegaLights; Shadows Stochastic): the view's local lights shaded at a FIXED
// price per pixel instead of every light of the froxel with its shadow map (no shadow map is drawn for them:
// MeshRenderer::UpdateLocalShadows). The light samples live at a DOWNSAMPLED resolution (Shadows.MegaLights
// DownsampleFactor, UE r.MegaLights.DownsampleFactor 2): one SAMPLE PIXEL per factor x factor block of render
// pixels, taken at the block's representative pixel (Common/MegaLights.glslh MegaLightsScreenPixel). Per frame,
// per view:
//  1) "MegaLights: Generate Samples"    - per sample pixel, Shadows.MegaLightsSamplesPerPixel reservoirs, each one
//     light of the pixel's light-grid cell drawn in proportion to its unshadowed contribution (no visibility);
//  2) "MegaLights: Temporal Resampling" - ReSTIR DI temporal reuse: the previous frame's TRACED reservoirs at the
//     sample pixel reprojected through the velocity, capped at Shadows.MegaLightsMaxHistory frames;
//  3) "MegaLights: Spatial Resampling"  - ReSTIR DI spatial reuse over Shadows.MegaLightsSpatialSamples
//  neighbours; 4) "MegaLights: Trace Samples"       - ONE visibility test per surviving sample (UE traces each
//  sample once):
//     the light's contact shadow through the G-buffer depth, then a ray query toward a point of its source sphere
//     (SourceRadius: soft shadows); W scaled by it. Next frame's history;
//  5) "MegaLights: Shade Samples"       - at RENDER resolution, every pixel shades its sample pixel's traced
//     reservoirs with its own surface (UE ShadeLightSamples: the upsample), the shading model's specular lobe and
//     its diffuse rest apart, each demodulated by its reflectance (Mesh/MegaLightsDemodulation.glslh);
//  6) the screen-space denoiser, two signals (UE MegaLights' diffuse and specular): DiffuseIndirect for the
//     diffuse, DirectSpecular (lobe-aware) for the specular (Shadows.MegaLightsDenoiserRadius) - the outputs the
//     deferred composite's DESERT_MEGALIGHTS program remodulates in place of its light-grid loop.
// The reservoir history (the reservoirs 4) wrote and the representative surface they were traced from) is this
// object's, per view: two slots, one read and one written per frame, created once at a size and extracted, as the
// denoiser's. Shaders (Editor/Resources/Shaders): Common/MegaLights.glslh, Programs/Shadows/MegaLights*.shader.
namespace Desert::Graphic::System
{
    inline constexpr std::string_view kMegaLightsDiffuseName  = "MegaLights.Diffuse";
    inline constexpr std::string_view kMegaLightsSpecularName = "MegaLights.Specular";
    // The most reservoirs a pixel keeps: the shaders' MEGALIGHTS_MAX_SAMPLES (Scalability 1..4).
    inline constexpr uint32_t kMegaLightsMaxSamplesPerPixel = 4;
    // The coarsest sample grid: one sample pixel per 2x2 render pixels (UE r.MegaLights.DownsampleFactor's range).
    inline constexpr uint32_t kMegaLightsMaxDownsampleFactor = 2;
    // The spatial reuse's disc, in render pixels (RTXDI's default spatial radius is 30 at 1080p; MegaLights reuses
    // within its downsampled tile neighbourhood): a constant of the method, not a setting.
    inline constexpr float kMegaLightsSpatialRadiusPixels = 16.0f;
    // UE r.RayTracing.NormalBias (0.1 cm), as the ray-traced sun.
    inline constexpr float kMegaLightsNormalBias = 0.1f;

    // Scalability Shadows.MegaLights*, taken each BeginScene.
    struct MegaLightsQuality
    {
        uint32_t SamplesPerPixel = 1; // 1..kMegaLightsMaxSamplesPerPixel
        uint32_t SpatialSamples  = 0; // 0 = no spatial reuse
        uint32_t MaxHistory      = 0; // frames; 0 = no temporal reuse
        uint32_t DenoiserRadius  = 2; // taps per side from the centre
        uint32_t DownsampleFactor =
             2; // render pixels per sample pixel, per side: 1..kMegaLightsMaxDownsampleFactor
    };

    // The denoised, demodulated local lighting the composite remodulates (rgb, straight), render sized.
    struct MegaLightsLighting
    {
        RDG::TextureRef Diffuse;
        RDG::TextureRef Specular;
    };

    // This frame's inputs, graph refs of this view's graph.
    struct MegaLightsInputs
    {
        RDG::TextureRef                     Depth;       // the G-buffer's device depth
        RDG::TextureRef                     GBufferA;    // albedo + metallic
        RDG::TextureRef                     GBufferB;    // normal + roughness
        RDG::TextureRef                     ShadingWord; // RG32_UINT
        RDG::TextureRef                     Velocity;    // FrameTransients::Velocity
        RDG::TextureRef                     Black;       // System.Black: the history before one exists
        RDG::BufferRef                      Tlas;
        RayTracing::RayTracingMetadataFrame Metadata;
        ViewLightBuffers                    Lights; // the rows and the light grid (AddFrameLightGrid)
        uint32_t                            PointLights = 0;
        uint32_t                            SpotLights  = 0;
        uint32_t                            Width       = 0; // render resolution
        uint32_t                            Height      = 0;
        glm::vec4 ContactShadowView{ 0.0f }; // SceneResources ShadowUBData::ContactShadowView (PackContactShadow)
    };

    // MegaLightsViewUB (Common/MegaLights.glslh), std140.
    struct MegaLightsViewUniforms
    {
        glm::mat4  ViewProjection;
        glm::mat4  PrevViewProjection;
        glm::mat4  InvJitteredViewProjection;
        glm::mat4  JitteredViewProjection; // the contact shadow's march (Common/ContactShadow.glslh)
        glm::vec4  CameraPosition;         // xyz
        glm::uvec4 Counts;                 // points, spots, samples per pixel, spatial samples
        glm::vec4  Params;            // history cap, history readable, normal bias, spatial radius (sample px)
        glm::uvec4 Frame;             // frame index, render width, render height, downsample factor
        glm::vec4  ContactShadowView; // steps, tan(fov / 2), dither frame
    };
    static_assert( sizeof( MegaLightsViewUniforms ) == 336 );

    class MegaLightsRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            for ( std::size_t stage = 0; stage < kStageCount; ++stage )
            {
                Stage&           program = m_Stages[stage];
                std::string_view name    = kShaderNames[stage];
                program.Shader           = Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
                if ( !program.Shader )
                    return Common::MakeError( std::format( "MegaLights: shader {} not found", name ) );
                const auto pipeline =
                     ComputePipeline::Create( { .Shader = program.Shader, .DebugName = std::string( name ) } );
                if ( !pipeline )
                    return Common::MakeError( pipeline.GetError() );
                program.Pipeline = pipeline.GetValue();
            }
            if ( const Common::BoolResultStr diffuse = m_DiffuseDenoiser.Initialize(); !diffuse )
                return diffuse;
            return m_SpecularDenoiser.Initialize();
        }

        // Declares the five MegaLights passes and the two denoiser signals; returns the denoised, demodulated
        // local lighting. An error, never an unshadowed light grid, when the frame has no TLAS or a pass could
        // not be declared.
        [[nodiscard]] Common::ResultStr<MegaLightsLighting> AddPasses( RDG::Builder&            graph,
                                                                       const MegaLightsInputs&  in,
                                                                       const MegaLightsQuality& quality,
                                                                       const ViewFrame&         frame )
        {
            for ( const Stage& stage : m_Stages )
                if ( !stage.Pipeline )
                    return Common::MakeError<MegaLightsLighting>(
                         "MegaLights: the pipelines are not initialised" );
            if ( !in.Tlas.IsValid() || !in.Metadata.Geometry.IsValid() || !in.Metadata.Materials.IsValid() )
                return Common::MakeError<MegaLightsLighting>(
                     "MegaLights: Shadows resolved to Stochastic but the frame has no TLAS" );
            if ( !in.Lights.GridCells.IsValid() || !in.Velocity.IsValid() )
                return Common::MakeError<MegaLightsLighting>( std::format(
                     "MegaLights: the frame has no {}",
                     in.Velocity.IsValid() ? "light grid" : "velocity to reproject the history with" ) );
            const uint32_t samples = std::clamp( quality.SamplesPerPixel, 1u, kMegaLightsMaxSamplesPerPixel );
            const uint32_t factor  = std::clamp( quality.DownsampleFactor, 1u, kMegaLightsMaxDownsampleFactor );
            const uint32_t width   = in.Width; // render: the shading and the denoiser
            const uint32_t height  = in.Height;
            // The sample pixels (Common/MegaLights.glslh MegaLightsSampleSize): the reservoirs and their history.
            const uint32_t sampleWidth  = ( width + factor - 1u ) / factor;
            const uint32_t sampleHeight = ( height + factor - 1u ) / factor;
            const uint32_t stacked      = sampleHeight * samples;

            // The history: readable when the previous frame of this view wrote it at this size and sample count.
            HistorySlot& previous = m_Slots[m_Parity];
            HistorySlot& next     = m_Slots[1u - m_Parity];
            const bool   readable = previous.Built && previous.Width == sampleWidth &&
                                  previous.Height == sampleHeight && previous.Samples == samples &&
                                  previous.Stamp.ReadableIn( frame );
            std::array<RDG::TextureRef, 2> history = { in.Black, in.Black };
            if ( readable )
                history = AcquireSlot( graph, previous, m_Parity, sampleWidth, sampleHeight, samples );
            const std::array<RDG::TextureRef, 2> written =
                 AcquireSlot( graph, next, 1u - m_Parity, sampleWidth, sampleHeight, samples );
            m_Parity = 1u - m_Parity;

            const MegaLightsViewUniforms uniforms{
                 .ViewProjection            = frame.ViewProjection,
                 .PrevViewProjection        = frame.PrevViewProjection,
                 .InvJitteredViewProjection = frame.InvJitteredViewProjection,
                 .JitteredViewProjection    = frame.JitteredViewProjection,
                 .CameraPosition            = glm::vec4( frame.CameraPosition, 1.0f ),
                 .Counts = glm::uvec4( in.PointLights, in.SpotLights, samples, quality.SpatialSamples ),
                 .Params = glm::vec4( static_cast<float>( quality.MaxHistory ), readable ? 1.0f : 0.0f,
                                      kMegaLightsNormalBias,
                                      kMegaLightsSpatialRadiusPixels / static_cast<float>( factor ) ),
                 .Frame  = glm::uvec4( static_cast<uint32_t>( frame.FrameIndex ), width, height, factor ),
                 .ContactShadowView = in.ContactShadowView };
            const RDG::BufferRef view =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( MegaLightsViewUniforms ) }, "MegaLights.ViewUB" );
            graph.QueueBufferUpload( view,
                                     std::as_bytes( std::span<const MegaLightsViewUniforms>( &uniforms, 1 ) ) );

            const RDG::TextureDesc reservoirDesc{ .Size   = { .Width = sampleWidth, .Height = stacked },
                                                  .Format = Core::Formats::ImageFormat::RGBA32F };
            const RDG::TextureRef  generated = graph.CreateTexture( reservoirDesc, "MegaLights.Samples" );
            const RDG::TextureRef  temporal  = graph.CreateTexture( reservoirDesc, "MegaLights.Samples.Temporal" );
            const RDG::TextureRef  spatial   = graph.CreateTexture( reservoirDesc, "MegaLights.Samples.Spatial" );
            const RDG::TextureDesc lightingDesc{ .Size   = { .Width = width, .Height = height },
                                                 .Format = ViewTargetFormats::kDenoiserSignal };
            const RDG::TextureRef  diffuse  = graph.CreateTexture( lightingDesc, kMegaLightsDiffuseName );
            const RDG::TextureRef  specular = graph.CreateTexture( lightingDesc, kMegaLightsSpecularName );
            for ( const RDG::TextureRef texture : { generated, temporal, spatial, diffuse, specular } )
                graph.SetFaultDefault( texture, RDG::FaultDefault::Black );

            const auto common = [&]( RDG::BindingBlockBuilder& block, const bool shading )
            {
                block.Uniform( "MegaLightsViewUB", view )
                     .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                     .Texture( "u_GBufferDepth", in.Depth, RDG::Access::SampledCompute,
                               RDG::SubresourceRange::All() )
                     .Texture( "u_GBufferNormal", in.GBufferB, RDG::Access::SampledCompute,
                               RDG::SubresourceRange::All() )
                     .Storage( ShaderProtocols::PointLight::Name, in.Lights.Point, RDG::Access::StorageRead )
                     .Storage( ShaderProtocols::SpotLight::Name, in.Lights.Spot, RDG::Access::StorageRead );
                if ( shading )
                    block.Texture( "u_GBufferA", in.GBufferA, RDG::Access::SampledCompute,
                                   RDG::SubresourceRange::All() )
                         .Texture( "u_GBufferShadingWord", in.ShadingWord, RDG::Access::SampledCompute,
                                   RDG::SubresourceRange::All() );
            };
            const auto tracing = [&]( RDG::BindingBlockBuilder& block )
            {
                block.AccelerationStructure( "u_SceneTLAS", in.Tlas )
                     .Storage( "RayTracingGeometryRecords", in.Metadata.Geometry, RDG::Access::StorageRead )
                     .Storage( "RayTracingMaterialRecords", in.Metadata.Materials, RDG::Access::StorageRead );
            };
            const auto read =
                 [&]( RDG::BindingBlockBuilder& block, std::string_view name, RDG::TextureRef texture )
            { block.Texture( name, texture, RDG::Access::SampledCompute, RDG::SubresourceRange::All() ); };

            AddStage( graph, kGenerate, sampleWidth, sampleHeight,
                      [&]( RDG::BindingBlockBuilder& block )
                      {
                          common( block, true );
                          block.Storage( kLightGridParamsBlockName, in.Lights.GridParams,
                                         RDG::Access::StorageRead )
                               .Storage( kLightGridCellsBlockName, in.Lights.GridCells, RDG::Access::StorageRead )
                               .Storage( "u_OutReservoirs", generated, RDG::Access::StorageWrite );
                      } );
            AddStage( graph, kTemporal, sampleWidth, sampleHeight,
                      [&]( RDG::BindingBlockBuilder& block )
                      {
                          common( block, true );
                          read( block, "u_Reservoirs", generated );
                          read( block, "u_HistoryReservoirs", history[0] );
                          read( block, "u_HistorySurface", history[1] );
                          read( block, "u_Velocity", in.Velocity );
                          block.Storage( "u_OutReservoirs", temporal, RDG::Access::StorageWrite );
                      } );
            AddStage( graph, kSpatial, sampleWidth, sampleHeight,
                      [&]( RDG::BindingBlockBuilder& block )
                      {
                          common( block, true );
                          read( block, "u_Reservoirs", temporal );
                          block.Storage( "u_OutReservoirs", spatial, RDG::Access::StorageWrite );
                      } );
            HistorySlot* stamped = &next;
            AddStage(
                 graph, kTrace, sampleWidth, sampleHeight,
                 [&]( RDG::BindingBlockBuilder& block )
                 {
                     common( block, true );
                     tracing( block );
                     read( block, "u_Reservoirs", spatial );
                     block.Storage( "u_OutReservoirs", written[0], RDG::Access::StorageWrite )
                          .Storage( "u_OutSurface", written[1], RDG::Access::StorageWrite );
                 },
                 [stamped, index = frame.FrameIndex]() { stamped->Stamp.Stamp( index ); } );
            AddStage( graph, kShade, width, height,
                      [&]( RDG::BindingBlockBuilder& block )
                      {
                          common( block, true );
                          read( block, "u_Reservoirs", written[0] );
                          block.Storage( "u_OutDiffuse", diffuse, RDG::Access::StorageWrite )
                               .Storage( "u_OutSpecular", specular, RDG::Access::StorageWrite );
                      } );

            const DenoiserSceneTextures scene{ .GBufferNormal = in.GBufferB,
                                               .GBufferDepth  = in.Depth,
                                               .Velocity      = in.Velocity,
                                               .Black         = in.Black };
            const MegaLightsLighting    denoised{
                    .Diffuse = m_DiffuseDenoiser.DenoiseDiffuseIndirect(
                      graph,
                      DiffuseIndirectDenoiserInputs{ .Signal               = diffuse,
                                                        .ReconstructionRadius = quality.DenoiserRadius },
                      scene, frame, width, height ),
                    .Specular = m_SpecularDenoiser.DenoiseDirectSpecular(
                      graph,
                      DirectSpecularDenoiserInputs{ .Signal               = specular,
                                                       .ReconstructionRadius = quality.DenoiserRadius },
                      scene, frame, width, height ) };
            if ( !denoised.Diffuse.IsValid() || !denoised.Specular.IsValid() )
                return Common::MakeError<MegaLightsLighting>( "MegaLights: the denoisers are not initialised" );
            return Common::MakeSuccess( denoised );
        }

    private:
        static constexpr std::size_t                               kGenerate    = 0;
        static constexpr std::size_t                               kTemporal    = 1;
        static constexpr std::size_t                               kSpatial     = 2;
        static constexpr std::size_t                               kTrace       = 3;
        static constexpr std::size_t                               kShade       = 4;
        static constexpr std::size_t                               kStageCount  = 5;
        static constexpr std::array<std::string_view, kStageCount> kShaderNames = {
             "MegaLightsGenerateSamples", "MegaLightsTemporalResampling", "MegaLightsSpatialResampling",
             "MegaLightsTraceSamples", "MegaLightsShadeSamples" };
        static constexpr std::array<std::string_view, kStageCount> kPassNames = {
             "MegaLights: Generate Samples", "MegaLights: Temporal Resampling", "MegaLights: Spatial Resampling",
             "MegaLights: Trace Samples", "MegaLights: Shade Samples" };
        // The two history slots' graph names (static: the graph keeps a view of each while it builds).
        static constexpr std::array<std::string_view, 2> kHistoryReservoirNames = { "MegaLights.History0",
                                                                                    "MegaLights.History1" };
        static constexpr std::array<std::string_view, 2> kHistorySurfaceNames   = { "MegaLights.History0.Surface",
                                                                                    "MegaLights.History1.Surface" };

        struct Stage
        {
            std::shared_ptr<Shader>          Shader;
            std::shared_ptr<ComputePipeline> Pipeline;
            mutable ShaderBindingLayoutCache Layout;
        };

        // One history slot, sample-pixel sized: the traced reservoirs (stacked layers) and the representative
        // surface they were traced from.
        struct HistorySlot
        {
            RDG::ExternalTexture Reservoirs;
            RDG::ExternalTexture Surface;
            bool                 Built   = false;
            uint32_t             Width   = 0;
            uint32_t             Height  = 0;
            uint32_t             Samples = 0;
            PassHistoryStamp     Stamp; // which view frame wrote it
        };

        // The slot's two textures in this graph: registered when they exist at this size, created and extracted
        // (kept past the graph, read by next frame's temporal resampling) when they do not.
        static std::array<RDG::TextureRef, 2> AcquireSlot( RDG::Builder& graph, HistorySlot& slot,
                                                           const uint32_t index, const uint32_t width,
                                                           const uint32_t height, const uint32_t samples )
        {
            if ( slot.Built && slot.Width == width && slot.Height == height && slot.Samples == samples )
                return { graph.RegisterExternal( slot.Reservoirs, kHistoryReservoirNames[index] ),
                         graph.RegisterExternal( slot.Surface, kHistorySurfaceNames[index] ) };
            slot.Reservoirs = {};
            slot.Surface    = {};
            slot.Stamp.Invalidate();
            const RDG::TextureRef reservoirs =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height * samples },
                                                        .Format = Core::Formats::ImageFormat::RGBA32F },
                                      kHistoryReservoirNames[index] );
            const RDG::TextureRef surface =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = Core::Formats::ImageFormat::RGBA16F },
                                      kHistorySurfaceNames[index] );
            graph.Extract( reservoirs, slot.Reservoirs, RDG::Access::SampledCompute );
            graph.Extract( surface, slot.Surface, RDG::Access::SampledCompute );
            slot.Built   = true;
            slot.Width   = width;
            slot.Height  = height;
            slot.Samples = samples;
            return { reservoirs, surface };
        }

        // One compute pass of @p stage over @p width x @p height (the sample pixels, or the render pixels for the
        // shading; LocalSize(8, 8, 1) in every MegaLights shader);
        // @p onDispatched runs after a successful dispatch (the history stamp).
        template <typename Declare, typename Done = void ( * )()>
        void AddStage(
             RDG::Builder& graph, const std::size_t stage, const uint32_t width, const uint32_t height,
             Declare&& declare, Done onDispatched = [] {} )
        {
            const Stage& program = m_Stages[stage];
            graph.AddPass(
                 kPassNames[stage], RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     auto block = pass.Bindings( program.Layout.Get( program.Shader ),
                                                 Renderer::GetPipelineRouteFill( *program.Pipeline ) );
                     declare( block );
                 },
                 [&program, width, height, onDispatched]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "MegaLights" );
                     const RDG::PassBindings     bindings( context, context.GetBindingBlock( 0 ) );
                     const Common::BoolResultStr dispatched = Renderer::GetInstance().DispatchCompute(
                          bindings, *program.Pipeline, ( width + 7u ) / 8u, ( height + 7u ) / 8u, 1 );
                     if ( dispatched.IsSuccess() )
                         onDispatched();
                     return dispatched;
                 } );
        }

        std::array<Stage, kStageCount> m_Stages;
        std::array<HistorySlot, 2>     m_Slots;
        uint32_t                       m_Parity = 0;
        ScreenSpaceDenoiser            m_DiffuseDenoiser{ DenoiserSignal::DiffuseIndirect };
        ScreenSpaceDenoiser            m_SpecularDenoiser{ DenoiserSignal::DirectSpecular };
    };
} // namespace Desert::Graphic::System

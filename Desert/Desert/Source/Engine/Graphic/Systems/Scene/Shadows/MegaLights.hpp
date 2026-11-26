#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/FrameKeptBuffer.hpp>
#include <Engine/Graphic/LightRowHistory.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/Systems/RayTracing/EmissiveLightsRenderer.hpp>
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
#include <string>
#include <string_view>
#include <Engine/Runtime/Services/Shader/ShaderLookup.hpp>

// MEGALIGHTS (UE 5.5 Renderer/Private/MegaLights; Shadows Stochastic): the view's local lights shaded at a FIXED
// price per pixel instead of every light of the froxel with its shadow map (no shadow map is drawn for them:
// MeshRenderer::UpdateLocalShadows). The light samples live at a DOWNSAMPLED resolution (Shadows.MegaLights
// DownsampleFactor, UE r.MegaLights.DownsampleFactor 2): one SAMPLE PIXEL per factor x factor block of render
// pixels, taken at the block's representative pixel (Common/MegaLights.glslh MegaLightsScreenPixel). Per frame,
// per view:
//  1) "MegaLights: Generate Samples"   - per sample pixel, Shadows.MegaLightsSamplesPerPixel samples, each one
//     light of the pixel's light-grid cell drawn in proportion to the perceived brightness of its unshadowed
//     contribution, a light the tile did not see last frame with a share of it (no visibility here);
//  2) "MegaLights: Trace Samples"      - ONE visibility test per sample (UE traces each sample once): the light's
//     contact shadow through the G-buffer depth, then the light's own visibility source (its payload's
//     ShadowVisibility, Common::Scalability::StochasticVisibility): a ray query toward a point of its
//     source sphere (SourceRadius: soft shadows), or the screen trace over the closest HZB followed by the
//     light's virtual shadow map pages (VSM-LOCAL), or the screen trace alone. One program per set of sources
//     present (Common::Scalability::StochasticSources, TraceProgramOf);
//  3) "MegaLights: Visible Light Hash" - per tile of kMegaLightsHashTile^2 sample pixels, the lights whose samples
//     were visible: what 1) is guided by next frame;
//  4) "MegaLights: Shade Samples"      - at RENDER resolution, every pixel shades the samples of one of its four
//     sample pixels with its own surface (UE ShadeLightSamples: the upsample), the shading model's specular lobe
//     and its diffuse rest apart, each demodulated by its reflectance (Mesh/MegaLightsDemodulation.glslh);
//  5) the screen-space denoiser, two signals (UE MegaLights' diffuse and specular): DiffuseIndirect for the
//     diffuse, DirectSpecular (lobe-aware) for the specular (Shadows.MegaLightsDenoiserRadius) - the outputs the
//     deferred composite's DESERT_MEGALIGHTS program remodulates in place of its light-grid loop.
// The samples live one frame; the only history of this object is the hash, per view: two slots, one read and one
// written per frame, created once at a size and extracted. Temporal accumulation is the denoiser's.
// Shaders (Editor/Resources/Shaders): Common/MegaLights.glslh, Programs/Shadows/MegaLights*.shader.
namespace Desert::Graphic::System
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MegaLightsShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, MegaLightsViewUB );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferNormalTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWordTexture );
        DESERT_SHADER_PARAMETER( AccelerationStructure, SceneTlas );
        DESERT_SHADER_PARAMETER( StorageBuffer, RayTracingGeometryRecords );
        DESERT_SHADER_PARAMETER( StorageBuffer, RayTracingMaterialRecords );
        DESERT_SHADER_PARAMETER( StorageTexture, OutSamplesTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, OutDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, OutSpecularTexture );
        DESERT_SHADER_PARAMETER( Texture, SamplesTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, PreviousLightRowsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, HistoryVisibleLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, VisibleLightsUB );
        DESERT_SHADER_PARAMETER( Texture, VelocityTexture );
        DESERT_SHADER_PARAMETER( Texture, HistoryCountTexture );
        DESERT_SHADER_PARAMETER( Texture, HzbTexture );
        // The scene-lighting emissive triangles (Common/MegaLights.glslh bindings 60..66).
        DESERT_SHADER_PARAMETER( StorageBuffer, MegaLightsEmissiveSegmentsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, MegaLightsEmissiveLookupUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, MegaLightsEmissiveTrianglesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, MegaLightsEmissivePowerUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, MegaLightsEmissiveDynamicSegmentsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, MegaLightsEmissiveDynamicTrianglesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, MegaLightsEmissiveDynamicPowerUB );
    };

    inline constexpr std::string_view kMegaLightsDiffuseName  = "MegaLights.Diffuse";
    inline constexpr std::string_view kMegaLightsSpecularName = "MegaLights.Specular";
    // The most samples a sample pixel draws: the shaders' MEGALIGHTS_MAX_SAMPLES (Scalability 1..4).
    inline constexpr uint32_t kMegaLightsMaxSamplesPerPixel = 4;
    // The coarsest sample grid: one sample pixel per 2x2 render pixels (UE r.MegaLights.DownsampleFactor's range).
    inline constexpr uint32_t kMegaLightsMaxDownsampleFactor = 2;
    // UE r.MegaLights.MaxShadingWeight / MaxShadingWeightForHiddenLight: the largest 1 / probability of choosing one
    // light of the pixel's candidates, so one improbable light is not a firefly. The energy above it is lost, as
    // UE's; the density an emissive triangle was drawn with is no part of it (Common/MegaLightsSample.glslh).
    inline constexpr float kMegaLightsMaxShadingWeight       = 20.0f;
    inline constexpr float kMegaLightsMaxHiddenShadingWeight = 5.0f;
    // The visible-light hash: one filter of kMegaLightsHashWords words per tile of this many sample pixels per side
    // (Common/MegaLightsSample.glslh MEGALIGHTS_HASH_TILE, uvec4).
    inline constexpr uint32_t kMegaLightsHashTile  = 8;
    inline constexpr uint32_t kMegaLightsHashWords = 4;
    // The share of its sampling weight a light the hash does not hold keeps (UE: its hidden-light sampling
    // weight). It moves the probability, not the value: the sample's weight divides by the same number.
    inline constexpr float kMegaLightsHiddenLightShare = 0.1f;
    // UE r.RayTracing.NormalBias (0.1 cm), as the ray-traced sun.
    inline constexpr float kMegaLightsNormalBias = 0.1f;

    // Scalability Shadows.MegaLights*, taken each BeginScene.
    struct MegaLightsQuality
    {
        uint32_t SamplesPerPixel  = 4; // 1..kMegaLightsMaxSamplesPerPixel
        uint32_t DenoiserRadius   = 2; // taps per side from the centre
        uint32_t DownsampleFactor = 2; // render pixels per sample pixel, per side: 1..kMegaLightsMaxDownsampleFactor
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
        RDG::TextureRef                     Black;       // System.Black: the denoisers' history before one exists
        // The visibility sources the frame's casting lights resolved to (never empty) and what they read: the TLAS
        // and its records (RayQuery), the closest HZB (VirtualMaps, Screen) and the virtual shadow map's reader
        // tables (VirtualMaps).
        Common::Scalability::StochasticSources Sources;
        RDG::BufferRef                      Tlas;
        RayTracing::RayTracingMetadataFrame Metadata;
        RDG::TextureRef                     Hzb;
        VirtualShadowMapReadRefs            Vsm;
        ViewLightBuffers                    Lights; // the rows, the light grid
        uint32_t                            PointLights = 0;
        uint32_t                            SpotLights  = 0;
        // The sources of the rows (LocalLightShadowSettings::SourceId), points and spots.
        std::span<const LocalLightShadowSettings> PointSources;
        std::span<const LocalLightShadowSettings> SpotSources;
        uint32_t                            Width       = 0; // render resolution
        uint32_t                            Height      = 0;
        glm::vec4 ContactShadowView{ 0.0f }; // SceneResources ShadowUBData::ContactShadowView (PackContactShadow)
        // THE SCENE-LIGHTING EMISSIVE TRIANGLES (SceneRenderer::AddFrameSceneEmissiveLights: the emissive list of
        // the materials with Emissive Lights Scene on): lights of the index space after the points and spots
        // (Common/MegaLights.glslh). Always a valid frame - with no such material its parts are empty (root
        // power 0) and no candidate is drawn.
        RayTracing::EmissiveLightsFrame Emissive;
    };

    // MegaLightsViewUB (Common/MegaLights.glslh), std140.
    struct MegaLightsViewUniforms
    {
        glm::mat4  ViewProjection;
        glm::mat4  PrevViewProjection;
        glm::mat4  InvJitteredViewProjection;
        glm::mat4  JitteredViewProjection; // the contact shadow's march (Common/ContactShadow.glslh)
        glm::vec4  CameraPosition;         // xyz, w = PreExposure
        glm::uvec4 Counts;                 // points, spots, samples per pixel, hash readable
        glm::vec4  Params;            // max shading weight, of a hidden light, normal bias, hidden light share
        glm::uvec4 Frame;             // frame index, render width, render height, downsample factor
        glm::vec4  ContactShadowView; // steps, tan(fov / 2), dither frame
        glm::uvec4 EmissiveCounts;    // EmissiveLightsFrame::StaticParams of MegaLightsInputs::Emissive
        glm::uvec4 EmissiveDynamicCounts; // and its DynamicParams
    };
    static_assert( sizeof( MegaLightsViewUniforms ) == 368 );

    class MegaLightsRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            // The trace's program is the frame's set of sources' (TraceProgramOf), built on its first frame
            // (EnsureTrace): a device without ray query never builds a ray-query one.
            for ( std::size_t stage = 0; stage < kStageCount; ++stage )
                if ( stage != kTrace )
                    if ( const Common::BoolResultStr built = Build( m_Stages[stage], kShaderNames[stage] ); !built )
                        return built;
            if ( const Common::BoolResultStr diffuse = m_DiffuseDenoiser.Initialize(); !diffuse )
                return diffuse;
            return m_SpecularDenoiser.Initialize();
        }

        // Declares the four MegaLights passes and the two denoiser signals; returns the denoised, demodulated
        // local lighting. An error, never an unshadowed light grid, when the frame has no TLAS or a pass could
        // not be declared.
        [[nodiscard]] Common::ResultStr<MegaLightsLighting> AddPasses( RDG::Builder&            graph,
                                                                       const MegaLightsInputs&  in,
                                                                       const MegaLightsQuality& quality,
                                                                       const ViewFrame&         frame )
        {
            for ( std::size_t stage = 0; stage < kStageCount; ++stage )
                if ( stage != kTrace && !m_Stages[stage].Pipeline )
                    return Common::MakeError<MegaLightsLighting>(
                         "MegaLights: the pipelines are not initialised" );
            if ( in.Sources.Empty() )
                return Common::MakeError<MegaLightsLighting>( "MegaLights: the frame names no visibility source" );
            const std::size_t program = TraceProgramOf( in.Sources );
            if ( const Common::BoolResultStr trace = EnsureTrace( program ); !trace )
                return Common::MakeError<MegaLightsLighting>( trace.GetError() );
            if ( in.Sources.RayQuery &&
                 ( !in.Tlas.IsValid() || !in.Metadata.Geometry.IsValid() || !in.Metadata.Materials.IsValid() ) )
                return Common::MakeError<MegaLightsLighting>(
                     "MegaLights: a light's visibility source is the ray query but the frame has no TLAS" );
            if ( in.Sources.ScreenTrace() && !in.Hzb.IsValid() )
                return Common::MakeError<MegaLightsLighting>(
                     "MegaLights: a light's visibility source traces the screen but the frame has no HZB" );
            if ( in.Sources.VirtualMaps && !in.Vsm.IsValid() )
                return Common::MakeError<MegaLightsLighting>(
                     "MegaLights: a light's visibility source is vsm+screen but the frame has no virtual shadow "
                     "map" );
            if ( !in.Emissive.Valid )
                return Common::MakeError<MegaLightsLighting>(
                     "MegaLights: the frame has no scene-lighting emissive list (AddFrameSceneEmissiveLights)" );
            if ( !in.Lights.GridCells.IsValid() || !in.Velocity.IsValid() )
                return Common::MakeError<MegaLightsLighting>( std::format(
                     "MegaLights: the frame has no {}",
                     in.Velocity.IsValid() ? "light grid" : "velocity to reproject the hash with" ) );
            // Read only where the hash is (Counts.w): the frame before it advanced these rows as well.
            const std::span<const int32_t> previousRows =
                 m_LightRows.Advance( in.PointSources, in.PointLights, in.SpotSources, in.SpotLights );
            const auto previousRowBuffer =
                 m_KeptPreviousRows.Register( graph, previousRows.size_bytes(), "MegaLights.PreviousRow" );
            if ( !previousRowBuffer )
                return Common::MakeError<MegaLightsLighting>( previousRowBuffer.GetError() );
            graph.QueueBufferUpload( previousRowBuffer.GetValue(), std::as_bytes( previousRows ) );
            const uint32_t samples = std::clamp( quality.SamplesPerPixel, 1u, kMegaLightsMaxSamplesPerPixel );
            const uint32_t factor  = std::clamp( quality.DownsampleFactor, 1u, kMegaLightsMaxDownsampleFactor );
            const uint32_t width   = in.Width; // render: the shading and the denoiser
            const uint32_t height  = in.Height;
            // The sample pixels (Common/MegaLights.glslh MegaLightsSampleSize): the samples and their hash.
            const uint32_t sampleWidth  = ( width + factor - 1u ) / factor;
            const uint32_t sampleHeight = ( height + factor - 1u ) / factor;
            const uint32_t stacked      = sampleHeight * samples;

            // The hash: readable when the previous frame of this view wrote it at this size.
            const uint32_t tilesWide   = ( sampleWidth + kMegaLightsHashTile - 1u ) / kMegaLightsHashTile;
            const uint32_t tilesHigh   = ( sampleHeight + kMegaLightsHashTile - 1u ) / kMegaLightsHashTile;
            const uint64_t hashBytes   = uint64_t( tilesWide ) * tilesHigh * kMegaLightsHashWords * sizeof( uint32_t );
            HistorySlot&   previous    = m_Slots[m_Parity];
            HistorySlot&   next        = m_Slots[1u - m_Parity];
            const bool     readable    = previous.Built && previous.Width == sampleWidth &&
                                  previous.Height == sampleHeight && previous.Stamp.ReadableIn( frame );
            RDG::BufferRef historyHash;
            if ( readable )
                historyHash = AcquireSlot( graph, previous, m_Parity, sampleWidth, sampleHeight, hashBytes );
            else
            {
                // Counts.w tells the shader not to look: the binding still names a buffer with contents.
                historyHash = graph.CreateBuffer(
                     RDG::BufferDesc{ uint64_t( kMegaLightsHashWords ) * sizeof( uint32_t ) }, "MegaLights.NoHash" );
                graph.QueueBufferClear( historyHash, 0u );
            }
            const RDG::BufferRef hash =
                 AcquireSlot( graph, next, 1u - m_Parity, sampleWidth, sampleHeight, hashBytes );
            m_Parity = 1u - m_Parity;

            // The surfaces the hash was made of: the diffuse denoiser's previous count and view depth. Without them
            // no reprojection into the hash can be told from a disocclusion.
            const DenoiserHistory diffuseHistory =
                 m_DiffuseDenoiser.AcquireHistory( graph, frame, in.Black, width, height );

            const MegaLightsViewUniforms uniforms{
                 .ViewProjection            = frame.ViewProjection,
                 .PrevViewProjection        = frame.PrevViewProjection,
                 .InvJitteredViewProjection = frame.InvJitteredViewProjection,
                 .JitteredViewProjection    = frame.JitteredViewProjection,
                 .CameraPosition            = glm::vec4( frame.CameraPosition, frame.PreExposure ),
                 .Counts = glm::uvec4( in.PointLights, in.SpotLights, samples,
                                      readable && diffuseHistory.Readable ? 1u : 0u ),
                 .Params = glm::vec4( kMegaLightsMaxShadingWeight, kMegaLightsMaxHiddenShadingWeight,
                                      kMegaLightsNormalBias, kMegaLightsHiddenLightShare ),
                 .Frame  = glm::uvec4( static_cast<uint32_t>( frame.FrameIndex ), width, height, factor ),
                 .ContactShadowView     = in.ContactShadowView,
                 .EmissiveCounts        = in.Emissive.StaticParams(),
                 .EmissiveDynamicCounts = in.Emissive.DynamicParams() };
            const RDG::BufferRef view =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( MegaLightsViewUniforms ) }, "MegaLights.ViewUB" );
            graph.QueueBufferUpload( view,
                                     std::as_bytes( std::span<const MegaLightsViewUniforms>( &uniforms, 1 ) ) );

            const RDG::TextureDesc sampleDesc{ .Size   = { .Width = sampleWidth, .Height = stacked },
                                               .Format = Core::Formats::ImageFormat::RG32_UINT };
            const RDG::TextureRef  generated = graph.CreateTexture( sampleDesc, "MegaLights.Samples" );
            const RDG::TextureRef  traced    = graph.CreateTexture( sampleDesc, "MegaLights.TracedSamples" );
            const RDG::TextureDesc lightingDesc{ .Size   = { .Width = width, .Height = height },
                                                 .Format = ViewTargetFormats::kDenoiserSignal };
            const RDG::TextureRef  diffuse  = graph.CreateTexture( lightingDesc, kMegaLightsDiffuseName );
            const RDG::TextureRef  specular = graph.CreateTexture( lightingDesc, kMegaLightsSpecularName );
            for ( const RDG::TextureRef texture : { diffuse, specular } )
                graph.SetFaultDefault( texture, RDG::FaultDefault::Black );

            const auto common = [&]( RDG::BindingBlockBuilder& block, const bool shading )
            {
                block.Uniform( MegaLightsShaderParameters::MegaLightsViewUB, view )
                     .Sampler( MegaLightsShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                     .Texture( MegaLightsShaderParameters::GBufferDepthTexture, in.Depth,
                               RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                     .Texture( MegaLightsShaderParameters::GBufferNormalTexture, in.GBufferB,
                               RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                     .Storage( ShaderProtocols::PointLight::Parameter, in.Lights.Point, RDG::Access::StorageRead )
                     .Storage( ShaderProtocols::SpotLight::Parameter, in.Lights.Spot, RDG::Access::StorageRead )
                     // ASYNC COMPUTE: the light function atlas the rows sample bindless, declared here so the
                     // graph orders its graphics-queue writer before this queue's read.
                     .BindlessRead( in.Lights.LightFunctionAtlas, RDG::Access::SampledCompute );
                using P          = MegaLightsShaderParameters;
                const auto& list = in.Emissive;
                constexpr RDG::Access kRead = RDG::Access::StorageRead;
                block.Storage( P::MegaLightsEmissiveSegmentsUB, list.Static.Segments, kRead )
                     .Storage( P::MegaLightsEmissiveLookupUB, list.Lookup, kRead )
                     .Storage( P::MegaLightsEmissiveTrianglesUB, list.Static.Triangles, kRead )
                     .Storage( P::MegaLightsEmissivePowerUB, list.Static.Power, kRead )
                     .Storage( P::MegaLightsEmissiveDynamicSegmentsUB, list.Dynamic.Segments, kRead )
                     .Storage( P::MegaLightsEmissiveDynamicTrianglesUB, list.Dynamic.Triangles, kRead )
                     .Storage( P::MegaLightsEmissiveDynamicPowerUB, list.Dynamic.Power, kRead );
                if ( shading )
                    block.Texture( MegaLightsShaderParameters::GBufferATexture, in.GBufferA,
                                   RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                         .Texture( MegaLightsShaderParameters::GBufferShadingWordTexture, in.ShadingWord,
                                   RDG::Access::SampledCompute, RDG::SubresourceRange::All() );
            };
            const Stage& traceProgram = m_Trace[program];
            const auto   tracing      = [&]( RDG::BindingBlockBuilder& block )
            {
                if ( in.Sources.RayQuery )
                    block.AccelerationStructure( MegaLightsShaderParameters::SceneTlas, in.Tlas )
                         .Storage( MegaLightsShaderParameters::RayTracingGeometryRecords, in.Metadata.Geometry,
                                   RDG::Access::StorageRead )
                         .Storage( MegaLightsShaderParameters::RayTracingMaterialRecords, in.Metadata.Materials,
                                   RDG::Access::StorageRead );
                if ( in.Sources.VirtualMaps )
                    BindVirtualShadowMapReads( block, in.Vsm,
                                               traceProgram.Layout.GetSlotted( traceProgram.Shader ).Slots );
                if ( in.Sources.ScreenTrace() )
                    block.Texture( MegaLightsShaderParameters::HzbTexture, in.Hzb, RDG::Access::SampledCompute,
                                   RDG::SubresourceRange::All() );
            };
            const auto read =
                 [&]( RDG::BindingBlockBuilder& block, RDG::TextureParameter name, RDG::TextureRef texture )
            { block.Texture( name, texture, RDG::Access::SampledCompute, RDG::SubresourceRange::All() ); };

            AddStage( graph, kGenerate, m_Stages[kGenerate], sampleWidth, sampleHeight,
                      [&]( RDG::BindingBlockBuilder& block )
                      {
                          common( block, true );
                          read( block, MegaLightsShaderParameters::VelocityTexture, in.Velocity );
                          read( block, MegaLightsShaderParameters::HistoryCountTexture,
                                diffuseHistory.SampleCount );
                          block.Storage( kLightGridParamsBlock, in.Lights.GridParams, RDG::Access::StorageRead )
                               .Storage( kLightGridCellsBlock, in.Lights.GridCells, RDG::Access::StorageRead )
                               .Storage( MegaLightsShaderParameters::PreviousLightRowsUB,
                                         previousRowBuffer.GetValue(), RDG::Access::StorageRead )
                               .Storage( MegaLightsShaderParameters::HistoryVisibleLightsUB, historyHash,
                                         RDG::Access::StorageRead )
                               .Storage( MegaLightsShaderParameters::OutSamplesTexture, generated,
                                         RDG::Access::StorageWrite );
                      } );
            AddStage( graph, kTrace, traceProgram, sampleWidth, sampleHeight,
                      [&]( RDG::BindingBlockBuilder& block )
                      {
                          common( block, true );
                          tracing( block );
                          read( block, MegaLightsShaderParameters::SamplesTexture, generated );
                          block.Storage( MegaLightsShaderParameters::OutSamplesTexture, traced,
                                         RDG::Access::StorageWrite );
                      } );
            HistorySlot* stamped = &next;
            AddStage(
                 graph, kVisibleHash, m_Stages[kVisibleHash], tilesWide, tilesHigh,
                 [&]( RDG::BindingBlockBuilder& block )
                 {
                     common( block, false );
                     read( block, MegaLightsShaderParameters::SamplesTexture, traced );
                     block.Storage( MegaLightsShaderParameters::VisibleLightsUB, hash, RDG::Access::StorageWrite );
                 },
                 [stamped, index = frame.FrameIndex]() { stamped->Stamp.Stamp( index ); } );
            AddStage( graph, kShade, m_Stages[kShade], width, height,
                      [&]( RDG::BindingBlockBuilder& block )
                      {
                          common( block, true );
                          read( block, MegaLightsShaderParameters::SamplesTexture, traced );
                          block.Storage( MegaLightsShaderParameters::OutDiffuseTexture, diffuse,
                                         RDG::Access::StorageWrite )
                               .Storage( MegaLightsShaderParameters::OutSpecularTexture, specular,
                                         RDG::Access::StorageWrite );
                      } );

            const DenoiserSceneTextures scene{ .GBufferNormal = in.GBufferB,
                                               .GBufferDepth  = in.Depth,
                                               .Velocity      = in.Velocity,
                                               .Black         = in.Black };
            const MegaLightsLighting    denoised{
                    .Diffuse = m_DiffuseDenoiser.DenoiseDiffuseIndirect(
                      graph,
                      DiffuseIndirectDenoiserInputs{ .Signal               = diffuse,
                                                        .ReconstructionRadius = quality.DenoiserRadius,
                                                        .History              = diffuseHistory },
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
        static constexpr std::size_t                               kTrace       = 1;
        static constexpr std::size_t                               kVisibleHash = 2;
        static constexpr std::size_t                               kShade       = 3;
        static constexpr std::size_t                               kStageCount  = 4;
        static constexpr std::array<std::string_view, kStageCount> kShaderNames = {
             "MegaLightsGenerateSamples", "MegaLightsTraceSamples", "MegaLightsVisibleHash",
             "MegaLightsShadeSamples" };
        // The trace's programs, one per set of visibility sources a frame can hold (TraceProgramOf). The mixed
        // ones branch per light on its payload's ShadowVisibility (Common/MegaLights.glslh MegaLightsVisibility).
        // A Screen light needs no program of its own beside vsm+screen: its slot is -1, the screen trace alone.
        static constexpr std::size_t                                      kTraceRayQuery               = 0;
        static constexpr std::size_t                                      kTraceVirtualMapsAndScreen   = 1;
        static constexpr std::size_t                                      kTraceScreen                 = 2;
        static constexpr std::size_t                                      kTraceRayQueryAndScreen      = 3;
        static constexpr std::size_t                                      kTraceRayQueryAndVirtualMaps = 4;
        static constexpr std::size_t                                      kTraceProgramCount           = 5;
        static constexpr std::array<std::string_view, kTraceProgramCount> kTraceShaderNames            = {
             "MegaLightsTraceSamples", "MegaLightsTraceSamplesVirtual", "MegaLightsTraceSamplesScreen",
             "MegaLightsTraceSamplesRayQueryScreen", "MegaLightsTraceSamplesRayQueryVirtual" };

        // The one trace program that holds every source of @p sources (not empty).
        [[nodiscard]] static constexpr std::size_t
        TraceProgramOf( const Common::Scalability::StochasticSources& sources )
        {
            if ( sources.RayQuery )
                return sources.VirtualMaps ? kTraceRayQueryAndVirtualMaps
                       : sources.Screen    ? kTraceRayQueryAndScreen
                                           : kTraceRayQuery;
            return sources.VirtualMaps ? kTraceVirtualMapsAndScreen : kTraceScreen;
        }
        static constexpr std::array<std::string_view, kStageCount> kPassNames = {
             "MegaLights: Generate Samples", "MegaLights: Trace Samples", "MegaLights: Visible Light Hash",
             "MegaLights: Shade Samples" };
        // The two hash slots' graph names (static: the graph keeps a view of each while it builds).
        static constexpr std::array<std::string_view, 2> kVisibleLightsNames = { "MegaLights.VisibleLights0",
                                                                                 "MegaLights.VisibleLights1" };

        struct Stage
        {
            std::shared_ptr<Shader>          Shader;
            std::shared_ptr<ComputePipeline> Pipeline;
            mutable ShaderBindingLayoutCache Layout;
        };

        [[nodiscard]] static Common::BoolResultStr Build( Stage& program, const std::string_view name )
        {
            program.Shader = Runtime::FindShader( std::string( name ) );
            if ( !program.Shader )
                return Common::MakeError( std::format( "MegaLights: shader {} not found", name ) );
            const auto pipeline =
                 ComputePipeline::Create( { .Shader = program.Shader, .DebugName = std::string( name ) } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            program.Pipeline = pipeline.GetValue();
            return Common::MakeSuccess( true );
        }

        // The trace program @p source (TraceProgramOf), built once; a failure is latched (no later frame repairs a
        // shader).
        [[nodiscard]] Common::BoolResultStr EnsureTrace( const std::size_t source )
        {
            if ( m_Trace[source].Pipeline )
                return Common::MakeSuccess( true );
            if ( !m_TraceErrors[source].empty() )
                return Common::MakeError( m_TraceErrors[source] );
            Common::BoolResultStr built = Build( m_Trace[source], kTraceShaderNames[source] );
            if ( !built )
                m_TraceErrors[source] = built.GetError();
            return built;
        }

        // One slot of the visible-light hash, for a sample grid of Width x Height.
        struct HistorySlot
        {
            RDG::ExternalBuffer VisibleLights;
            bool                Built  = false;
            uint32_t            Width  = 0;
            uint32_t            Height = 0;
            PassHistoryStamp    Stamp; // which view frame wrote it
        };

        // The slot's buffer in this graph: registered when it exists at this size, created and extracted (kept past
        // the graph, read by the next frame's sampling) when it does not.
        static RDG::BufferRef AcquireSlot( RDG::Builder& graph, HistorySlot& slot, const uint32_t index,
                                           const uint32_t width, const uint32_t height, const uint64_t bytes )
        {
            if ( slot.Built && slot.Width == width && slot.Height == height )
                return graph.RegisterExternal( slot.VisibleLights, kVisibleLightsNames[index] );
            slot.VisibleLights = {};
            slot.Stamp.Invalidate();
            const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ bytes }, kVisibleLightsNames[index] );
            graph.Extract( buffer, slot.VisibleLights, RDG::Access::StorageRead );
            slot.Built  = true;
            slot.Width  = width;
            slot.Height = height;
            return buffer;
        }

        // One compute pass of @p stage over @p width x @p height (the sample pixels, the hash tiles, or the render
        // pixels for the shading; LocalSize(8, 8, 1) in every MegaLights shader);
        // @p onDispatched runs after a successful dispatch (the history stamp).
        template <typename Declare, typename Done = void ( * )()>
        void AddStage(
             RDG::Builder& graph, const std::size_t stage, const Stage& program, const uint32_t width,
             const uint32_t height, Declare&& declare, Done onDispatched = [] {} )
        {
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

        std::array<Stage, kStageCount> m_Stages; // [kTrace] unused: the trace is m_Trace's
        std::array<Stage, kTraceProgramCount> m_Trace;  // per TraceProgramOf, built on first use (EnsureTrace)
        std::array<std::string, kTraceProgramCount> m_TraceErrors;
        std::array<HistorySlot, 2>     m_Slots;
        uint32_t                       m_Parity = 0;
        LightRowHistory                m_LightRows; // of this view's frames that ran MegaLights
        FrameKeptBuffer                m_KeptPreviousRows;
        ScreenSpaceDenoiser            m_DiffuseDenoiser{ DenoiserSignal::DiffuseIndirect };
        ScreenSpaceDenoiser            m_SpecularDenoiser{ DenoiserSignal::DirectSpecular };
    };
} // namespace Desert::Graphic::System

#pragma once

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>
#include <Engine/Graphic/DistanceField/MeshDistanceFieldReads.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// "Shadows: Distance Field Sun" (UE FProjectedShadowInfo::RenderRayTracedDistanceFieldProjection, r.DFShadow*):
// the sun past its last cascade. Four compute nodes over this frame's mesh distance fields and global distance
// field (DistanceField::MeshDistanceFieldReads, GlobalDistanceField::FrameField): the light-space tile cull
// (count, the counts' prefix sum, scatter - Programs/Shadows/DistanceFieldShadowCull*.shader), the half-resolution
// cone trace (DistanceFieldShadowTrace.shader) and the bilateral upsample to the render extent
// (DistanceFieldShadowUpsample.shader), into the screen mask the deferred direct lighting cross-fades with the
// cascades across the band (Common/DistanceFieldShadow.glslh, DeferredLighting.shader
// DesertSunShadowWithDistanceField). Read only by the deferred composite's cascade arm (CompositeView.x = 1).
namespace Desert::Graphic::System
{
    // UE r.DFShadow tile grid: the light-space grid's tiles a side (the grid is fitted per frame to the view).
    inline constexpr uint32_t kDistanceFieldShadowTilesPerSide = 64;
    inline constexpr uint32_t kDistanceFieldShadowTiles =
         kDistanceFieldShadowTilesPerSide * kDistanceFieldShadowTilesPerSide;
    // UE GAverageObjectsPerShadowCullTile (128): the FIRST frame's list entries' room per tile. A frame whose
    // lists need more is refused whole on the GPU (DistanceFieldShadowCullOffsets.shader: every list empty) -
    // never cut short - and reports what it needed (the feedback buffer); the renderer grows the room to that plus
    // a quarter for the frames after (DistanceFieldShadowRenderer::ReadFeedback), never shrinks it.
    inline constexpr uint32_t kDistanceFieldShadowObjectsPerTile = 128;
    inline constexpr uint32_t kDistanceFieldShadowInitialListRoom =
         kDistanceFieldShadowTiles * kDistanceFieldShadowObjectsPerTile;
    // Common/DistanceFieldShadow.glslh DistanceFieldShadowHeaderStride: per tile its list's first index, length.
    inline constexpr uint32_t kDistanceFieldShadowHeaderStride = 2;
    inline constexpr uint32_t kDistanceFieldShadowHeaderUints =
         kDistanceFieldShadowTiles * kDistanceFieldShadowHeaderStride;
    // DistanceFieldShadowCullOffsets.shader Feedback[]: needed entries, the room, the longest list, the stamp.
    inline constexpr uint32_t kDistanceFieldShadowFeedbackUints = 4;
    // UE DistanceFieldShadowDistance (300 m): the receivers' farthest distance from the camera, in cm.
    inline constexpr float kDistanceFieldShadowDistance = 30000.0f;
    // Common/DistanceFieldShadow.glslh kDistanceFieldShadowBand: the cross-fade's width in the last cascade's UV.
    inline constexpr float kDistanceFieldShadowBand = 0.10f;

    inline constexpr std::string_view kDistanceFieldShadowHalfName = "ShadowMask.Sun.DistanceField.Half";
    inline constexpr std::string_view kDistanceFieldShadowName     = "ShadowMask.Sun.DistanceField";

    // std430 `struct DistanceFieldShadowParams` (Common/DistanceFieldShadow.glslh), field for field.
    struct DistanceFieldShadowGpuParams
    {
        glm::vec4  LightX{ 1.0f, 0.0f, 0.0f, 0.0f }; // xyz = light-space x axis (unit), w = -dot(axis, camera)
        glm::vec4  LightY{ 0.0f, 1.0f, 0.0f, 0.0f };
        glm::vec4  LightZ{ 0.0f, 0.0f, 1.0f, 0.0f }; // toward the sun
        glm::vec4  Tiles{ 0.0f };                    // xy = the grid's min corner (cm), zw = 1 / tile size (1/cm)
        glm::vec4  Receivers{ 0.0f };                // lowest light z, tan(angle / 2), max distance, band
        glm::uvec4 Counts{ 0u };                     // objects (meshes + tiles), tiles a side, tiles, list room
        glm::mat4  InvViewProjection{ 1.0f };        // the G-buffer's inverse jittered view-projection
        glm::mat4  LastCascade{ 1.0f };              // the sun's last cascade's view-projection
        glm::vec4  Camera{ 0.0f };                   // xyz = camera (cm), w = 1 when LastCascade is the sun's
        std::array<glm::vec4, DistanceField::kGlobalDistanceFieldClipmaps> GdfLevels{};    // centre.xyz, voxel cm
        glm::vec4                                                          Extent{ 0.0f }; // full xy, half zw
    };
    static_assert( sizeof( DistanceFieldShadowGpuParams ) == 320 );

    // The slot names each node binds, spelled once (RDGShaderParameters.hpp), checked against the shaders in
    // Initialize.
    struct DistanceFieldShadowCullShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, DFObjects );
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowTilesUB );
        static constexpr std::array kAll{ RDG::Erase( DistanceFieldShadowParamsUB ), RDG::Erase( DFObjects ),
                                          RDG::Erase( DistanceFieldShadowTilesUB ) };
    };

    struct DistanceFieldShadowCullOffsetsShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowTilesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowFeedbackUB );
        static constexpr std::array kAll{ RDG::Erase( DistanceFieldShadowParamsUB ),
                                          RDG::Erase( DistanceFieldShadowTilesUB ),
                                          RDG::Erase( DistanceFieldShadowFeedbackUB ) };
    };

    struct DistanceFieldShadowTraceShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowTilesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, DFAssetData );
        DESERT_SHADER_PARAMETER( StorageBuffer, DFIndirection );
        DESERT_SHADER_PARAMETER( StorageBuffer, DFObjects );
        DESERT_SHADER_PARAMETER( Texture, DistanceFieldBrickAtlasTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DistanceFieldShadowHalfTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        static constexpr std::array kAll{ RDG::Erase( DistanceFieldShadowParamsUB ),
                                          RDG::Erase( DistanceFieldShadowTilesUB ),
                                          RDG::Erase( DFAssetData ),
                                          RDG::Erase( DFIndirection ),
                                          RDG::Erase( DFObjects ),
                                          RDG::Erase( DistanceFieldBrickAtlasTexture ),
                                          RDG::Erase( DistanceField::kGlobalDistanceFieldClipmapTextures[0] ),
                                          RDG::Erase( DistanceField::kGlobalDistanceFieldClipmapTextures[1] ),
                                          RDG::Erase( DistanceField::kGlobalDistanceFieldClipmapTextures[2] ),
                                          RDG::Erase( DistanceField::kGlobalDistanceFieldClipmapTextures[3] ),
                                          RDG::Erase( GBufferDepthTexture ),
                                          RDG::Erase( GBufferNormalTexture ),
                                          RDG::Erase( DistanceFieldShadowHalfTexture ),
                                          RDG::Erase( PointClampSampler ),
                                          RDG::Erase( LinearRepeatSampler ) };
    };

    struct DistanceFieldShadowUpsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, DistanceFieldShadowParamsUB );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, DistanceFieldShadowHalfTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DistanceFieldShadowTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        static constexpr std::array kAll{
             RDG::Erase( DistanceFieldShadowParamsUB ), RDG::Erase( GBufferDepthTexture ),
             RDG::Erase( DistanceFieldShadowHalfTexture ), RDG::Erase( DistanceFieldShadowTexture ),
             RDG::Erase( PointClampSampler ) };
    };

    // The push block of DistanceFieldShadowCull.shader.
    struct DistanceFieldShadowCullPush
    {
        uint32_t CullPass = 0; // 0 = count into the headers, 1 = scatter into the lists
    };
    static_assert( sizeof( DistanceFieldShadowCullPush ) == 4 );

    struct DistanceFieldShadowOffsetsPush
    {
        uint32_t Stamp = 0; // this cull's stamp, written into the feedback beside what the lists needed
    };
    static_assert( sizeof( DistanceFieldShadowOffsetsPush ) == 4 );

    // This frame's view and fields, gathered by SceneRenderer::AddFrameDistanceFieldShadow.
    struct DistanceFieldShadowInputs
    {
        RDG::TextureRef                                                          Depth;  // the G-buffer's
        RDG::TextureRef                                                          Normal; // the G-buffer's
        DistanceField::MeshDistanceFieldReads                                    Fields;
        std::array<RDG::TextureRef, DistanceField::kGlobalDistanceFieldClipmaps> Clipmaps;
        std::array<glm::vec4, DistanceField::kGlobalDistanceFieldClipmaps>       Levels{};
        glm::mat4                                                                InvJitteredViewProjection{ 1.0f };
        glm::vec3                                                                CameraPosition{ 0.0f };
        glm::vec3        DirectionToSun{ 0.0f, 1.0f, 0.0f };
        float            SourceTanHalf = 0.0f;    // DirectionalShadowView::SourceTanHalf of the sun
        const glm::mat4* LastCascade   = nullptr; // the sun's last cascade (receiver side); null = it has none
        uint32_t         Width         = 0;       // the render extent
        uint32_t         Height        = 0;
    };

    // The light basis, the tile grid fitted to the view and the record every node reads. The grid is the
    // light-space box of the camera and of nine view rays (the corners, the edges' middles and the centre of the
    // screen, at device depth 1 - the near plane of the reversed-Z projection - so an infinite far plane still
    // gives their directions) carried kDistanceFieldShadowDistance out, plus one tile of margin each side; the
    // lowest receiver is the lowest of those points along the light's z. `listRoom`: the tile lists' entries the
    // frame's tile buffer has after the headers (DistanceFieldShadowRenderer's grown room).
    [[nodiscard]] inline DistanceFieldShadowGpuParams
    MakeDistanceFieldShadowGpuParams( const DistanceFieldShadowInputs& in, const uint32_t listRoom )
    {
        DistanceFieldShadowGpuParams gpu;
        const glm::vec3              z  = glm::normalize( in.DirectionToSun );
        glm::vec3                    up = glm::vec3( 0.0f, 1.0f, 0.0f );
        if ( std::abs( z.y ) > 0.99f )
            up = glm::vec3( 1.0f, 0.0f, 0.0f );
        const glm::vec3 x      = glm::normalize( glm::cross( z, up ) );
        const glm::vec3 y      = glm::cross( x, z );
        const glm::vec3 camera = in.CameraPosition;
        gpu.LightX             = glm::vec4( x, -glm::dot( x, camera ) );
        gpu.LightY             = glm::vec4( y, -glm::dot( y, camera ) );
        gpu.LightZ             = glm::vec4( z, -glm::dot( z, camera ) );

        // The camera is the light space's origin.
        glm::vec2 lo( 0.0f );
        glm::vec2 hi( 0.0f );
        float     lowest = 0.0f;
        for ( int iy = -1; iy <= 1; ++iy )
            for ( int ix = -1; ix <= 1; ++ix )
            {
                const glm::vec4 clip( static_cast<float>( ix ), static_cast<float>( iy ), 1.0f, 1.0f );
                const glm::vec4 world = in.InvJitteredViewProjection * clip;
                const glm::vec3 point = glm::vec3( world ) / world.w;
                const glm::vec3 ray   = glm::normalize( point - camera ) * kDistanceFieldShadowDistance;
                const glm::vec3 light( glm::dot( x, ray ), glm::dot( y, ray ), glm::dot( z, ray ) );
                lo     = glm::min( lo, glm::vec2( light ) );
                hi     = glm::max( hi, glm::vec2( light ) );
                lowest = std::min( lowest, light.z );
            }
        constexpr float kInner = static_cast<float>( kDistanceFieldShadowTilesPerSide - 2u );
        const glm::vec2 tile   = glm::max( ( hi - lo ) / kInner, glm::vec2( 1.0f ) );
        gpu.Tiles              = glm::vec4( lo - tile, 1.0f / tile );
        gpu.Receivers =
             glm::vec4( lowest, in.SourceTanHalf, kDistanceFieldShadowDistance, kDistanceFieldShadowBand );
        gpu.Counts            = glm::uvec4( in.Fields.NumObjects, kDistanceFieldShadowTilesPerSide,
                                            kDistanceFieldShadowTiles, listRoom );
        gpu.InvViewProjection = in.InvJitteredViewProjection;
        float hasCascade      = 0.0f;
        if ( in.LastCascade != nullptr )
        {
            gpu.LastCascade = *in.LastCascade;
            hasCascade      = 1.0f;
        }
        gpu.Camera                = glm::vec4( camera, hasCascade );
        gpu.GdfLevels             = in.Levels;
        const uint32_t halfWidth  = ( in.Width + 1u ) / 2u;
        const uint32_t halfHeight = ( in.Height + 1u ) / 2u;
        gpu.Extent                = glm::vec4( static_cast<float>( in.Width ), static_cast<float>( in.Height ),
                                               static_cast<float>( halfWidth ), static_cast<float>( halfHeight ) );
        return gpu;
    }

    class DistanceFieldShadowRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            if ( const auto made =
                      Make( m_Cull, "DistanceFieldShadowCull", DistanceFieldShadowCullShaderParameters::kAll );
                 !made )
                return made;
            if ( const auto made = Make( m_Offsets, "DistanceFieldShadowCullOffsets",
                                         DistanceFieldShadowCullOffsetsShaderParameters::kAll );
                 !made )
                return made;
            if ( const auto made =
                      Make( m_Trace, "DistanceFieldShadowTrace", DistanceFieldShadowTraceShaderParameters::kAll );
                 !made )
                return made;
            return Make( m_Upsample, "DistanceFieldShadowUpsample",
                         DistanceFieldShadowUpsampleShaderParameters::kAll );
        }

        // Declares the five nodes (cull count, offsets, cull scatter, trace, upsample) and returns the mask at
        // the render extent (r = visibility, 1 = lit). An error, never a mask nothing wrote, when an input of the
        // frame is missing.
        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPasses( RDG::Builder&                    graph,
                                                                    const DistanceFieldShadowInputs& in )
        {
            if ( !m_Upsample.Pipeline )
                return Common::MakeError<RDG::TextureRef>(
                     "Shadows: Distance Field Sun: the pipelines are not initialised" );
            const DistanceField::MeshDistanceFieldReads& fields = in.Fields;
            if ( !fields.Assets.IsValid() || !fields.Indirection.IsValid() || !fields.Objects.IsValid() ||
                 !fields.BrickAtlas.IsValid() )
                return Common::MakeError<RDG::TextureRef>(
                     "Shadows: Distance Field Sun: the frame's mesh distance fields were not imported" );
            for ( const RDG::TextureRef clipmap : in.Clipmaps )
                if ( !clipmap.IsValid() )
                    return Common::MakeError<RDG::TextureRef>(
                         "Shadows: Distance Field Sun: the frame's global distance field was not brought up" );
            if ( !in.Depth.IsValid() || !in.Normal.IsValid() || in.Width == 0 || in.Height == 0 )
                return Common::MakeError<RDG::TextureRef>(
                     "Shadows: Distance Field Sun: the view has no G-buffer depth and normal" );

            // The lists' room: what earlier frames needed (ReadFeedback, once this frame slot's fence was
            // waited), at least the first frame's 128 a tile. It must fit the shaders' uint indices.
            const uint32_t slot  = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
            const uint32_t slots = Engine::FrameManager::GetInstance().GetMaxFramesInFlight();
            if ( m_Feedback.size() != slots )
            {
                m_Feedback.assign( slots, RDG::ExternalBuffer{} );
                m_FeedbackStamp.assign( slots, 0u );
            }
            if ( slot >= slots )
                return Common::MakeFormattedError<RDG::TextureRef>(
                     "Shadows: Distance Field Sun: frame slot {} of {}", slot, slots );
            ReadFeedback( slot );
            const uint64_t tileUints = uint64_t{ kDistanceFieldShadowHeaderUints } + m_ListRoom;
            if ( tileUints > std::numeric_limits<uint32_t>::max() )
                return Common::MakeFormattedError<RDG::TextureRef>(
                     "Shadows: Distance Field Sun: {} tile list entries do not fit 32-bit indices", m_ListRoom );
            m_Stamp = m_Stamp == std::numeric_limits<uint32_t>::max() ? 1u : m_Stamp + 1u;

            const DistanceFieldShadowGpuParams gpu =
                 MakeDistanceFieldShadowGpuParams( in, static_cast<uint32_t>( m_ListRoom ) );
            const RDG::BufferRef params =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( gpu ) }, "ShadowMask.Sun.DistanceField.Params" );
            graph.QueueBufferUpload( params, std::as_bytes( std::span( &gpu, 1 ) ) );
            const RDG::BufferRef tiles =
                 graph.CreateBuffer( RDG::BufferDesc{ tileUints * 4ull }, "ShadowMask.Sun.DistanceField.Tiles" );
            const RDG::BufferRef feedback =
                 graph.CreateBuffer( RDG::BufferDesc{ uint64_t{ kDistanceFieldShadowFeedbackUints } * 4ull },
                                     "ShadowMask.Sun.DistanceField.Feedback" );
            const uint32_t        halfWidth  = ( in.Width + 1u ) / 2u;
            const uint32_t        halfHeight = ( in.Height + 1u ) / 2u;
            const RDG::TextureRef half =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = halfWidth, .Height = halfHeight },
                                                        .Format = ViewTargetFormats::kDenoiserSignal },
                                      kDistanceFieldShadowHalfName );
            const RDG::TextureRef mask =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = in.Width, .Height = in.Height },
                                                        .Format = ViewTargetFormats::kDenoiserSignal },
                                      kDistanceFieldShadowName );
            graph.SetFaultDefault( half, RDG::FaultDefault::White );
            graph.SetFaultDefault( mask, RDG::FaultDefault::White );

            constexpr RDG::Access kR = RDG::Access::StorageRead;
            constexpr RDG::Access kW = RDG::Access::StorageWrite;
            constexpr RDG::Access kS = RDG::Access::SampledCompute;

            // 1, 3: the tile cull - a thread per tile, counting (pass 0) then scattering (pass 1).
            const auto addCull = [&]( const std::string_view name, const uint32_t cullPass )
            {
                ComputePipeline* const            pipeline = m_Cull.Pipeline.get();
                const DistanceFieldShadowCullPush push{ cullPass };
                graph.AddPass(
                     name, RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_Cull.Layout.Get( m_Cull.Shader ),
                                        Renderer::GetPipelineRouteFill( *pipeline ) )
                              .Storage( DistanceFieldShadowCullShaderParameters::DistanceFieldShadowParamsUB,
                                        params, kR )
                              .Storage( DistanceFieldShadowCullShaderParameters::DFObjects, fields.Objects, kR )
                              .Storage( DistanceFieldShadowCullShaderParameters::DistanceFieldShadowTilesUB, tiles,
                                        kW )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( DistanceFieldShadowCullPush ) ) );
                     },
                     [pipeline, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "Shadows: Distance Field Sun: Cull" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                         return Renderer::GetInstance().DispatchCompute(
                              bindings, *pipeline, ( kDistanceFieldShadowTiles + 63u ) / 64u, 1u, 1u );
                     } );
            };
            addCull( "Shadows: Distance Field Sun: Cull Count", 0u );

            // 2: the counts' prefix sum into the lists' first indices (one workgroup), and the feedback.
            {
                ComputePipeline* const               pipeline = m_Offsets.Pipeline.get();
                const DistanceFieldShadowOffsetsPush push{ m_Stamp };
                graph.AddPass(
                     "Shadows: Distance Field Sun: Cull Offsets", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_Offsets.Layout.Get( m_Offsets.Shader ),
                                        Renderer::GetPipelineRouteFill( *pipeline ) )
                              .Storage(
                                   DistanceFieldShadowCullOffsetsShaderParameters::DistanceFieldShadowParamsUB,
                                   params, kR )
                              .Storage( DistanceFieldShadowCullOffsetsShaderParameters::DistanceFieldShadowTilesUB,
                                        tiles, kW )
                              .Storage(
                                   DistanceFieldShadowCullOffsetsShaderParameters::DistanceFieldShadowFeedbackUB,
                                   feedback, kW )
                              .PushConstantBytes(
                                   static_cast<uint32_t>( sizeof( DistanceFieldShadowOffsetsPush ) ) );
                     },
                     [pipeline, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "Shadows: Distance Field Sun: Cull Offsets" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, 1u, 1u, 1u );
                     } );
            }
            graph.Extract( feedback, m_Feedback[slot], RDG::Access::HostRead );
            m_FeedbackStamp[slot] = m_Stamp;
            addCull( "Shadows: Distance Field Sun: Cull Scatter", 1u );

            // 4: the half-resolution cone trace.
            {
                using P                         = DistanceFieldShadowTraceShaderParameters;
                ComputePipeline* const pipeline = m_Trace.Pipeline.get();
                graph.AddPass(
                     "Shadows: Distance Field Sun: Trace", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         auto block = pass.Bindings( m_Trace.Layout.Get( m_Trace.Shader ),
                                                     Renderer::GetPipelineRouteFill( *pipeline ) );
                         block.Storage( P::DistanceFieldShadowParamsUB, params, kR )
                              .Storage( P::DistanceFieldShadowTilesUB, tiles, kR )
                              .Storage( P::DFAssetData, fields.Assets, kR )
                              .Storage( P::DFIndirection, fields.Indirection, kR )
                              .Storage( P::DFObjects, fields.Objects, kR )
                              .Texture( P::DistanceFieldBrickAtlasTexture, fields.BrickAtlas, kS,
                                        RDG::SubresourceRange::All() )
                              .Texture( P::GBufferDepthTexture, in.Depth, kS, RDG::SubresourceRange::All() )
                              .Texture( P::GBufferNormalTexture, in.Normal, kS, RDG::SubresourceRange::All() )
                              .Storage( P::DistanceFieldShadowHalfTexture, half, kW )
                              .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                              .Sampler( P::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() );
                         for ( std::size_t level = 0; level < in.Clipmaps.size(); ++level )
                             block.Texture( DistanceField::kGlobalDistanceFieldClipmapTextures[level],
                                            in.Clipmaps[level], kS, RDG::SubresourceRange::All() );
                     },
                     [pipeline, halfWidth, halfHeight]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "Shadows: Distance Field Sun: Trace" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::GetInstance().DispatchCompute(
                              bindings, *pipeline, ( halfWidth + 7u ) / 8u, ( halfHeight + 7u ) / 8u, 1u );
                     } );
            }

            // 5: the bilateral upsample to the render extent.
            {
                using P                         = DistanceFieldShadowUpsampleShaderParameters;
                ComputePipeline* const pipeline = m_Upsample.Pipeline.get();
                const uint32_t         width    = in.Width;
                const uint32_t         height   = in.Height;
                graph.AddPass(
                     "Shadows: Distance Field Sun: Upsample", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_Upsample.Layout.Get( m_Upsample.Shader ),
                                        Renderer::GetPipelineRouteFill( *pipeline ) )
                              .Storage( P::DistanceFieldShadowParamsUB, params, kR )
                              .Texture( P::GBufferDepthTexture, in.Depth, kS, RDG::SubresourceRange::All() )
                              .Texture( P::DistanceFieldShadowHalfTexture, half, kS, RDG::SubresourceRange::All() )
                              .Storage( P::DistanceFieldShadowTexture, mask, kW )
                              .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() );
                     },
                     [pipeline, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "Shadows: Distance Field Sun: Upsample" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, ( width + 7u ) / 8u,
                                                                         ( height + 7u ) / 8u, 1u );
                     } );
            }
            return Common::MakeSuccess( mask );
        }

    private:
        // What the offsets pass of frame slot @p slot handed back, read once that slot comes round again (its
        // fence waited): grows m_ListRoom to what the lists needed plus a quarter and logs a refused frame once
        // per room. A readback of another cull than the slot's last (its stamp) is ignored.
        void ReadFeedback( const uint32_t slot )
        {
            RDG::ExternalBuffer& readback = m_Feedback[slot];
            const uint32_t       stamp    = std::exchange( m_FeedbackStamp[slot], 0u );
            if ( stamp == 0u || !readback.Physical )
                return; // no cull ran in this slot yet, or its graph faulted before the extract
            const std::span<const std::byte>                        bytes = readback.Physical->ReadForHost();
            std::array<uint32_t, kDistanceFieldShadowFeedbackUints> values{};
            if ( bytes.size() < sizeof( values ) )
            {
                LOG_ERROR(
                     "[Shadows] Distance Field Sun: the tile cull's feedback is not host-readable ({} bytes)",
                     bytes.size() );
                return;
            }
            std::memcpy( values.data(), bytes.data(), sizeof( values ) );
            readback.Physical.reset();
            const auto [needed, room, longest, written] = values;
            if ( written != stamp )
                return; // another cull's bytes: nothing to learn from them
            if ( !m_FeedbackLogged )
            {
                m_FeedbackLogged = true;
                LOG_INFO( "[Shadows] Distance Field Sun tile lists: {} entries, the longest {}, room for {}",
                          needed, longest, room );
            }
            if ( needed <= room )
                return;
            m_ListRoom = std::max( m_ListRoom, uint64_t{ needed } + uint64_t{ needed } / 4u );
            // The other slots culled with the same room report the same refusal: one line per room.
            if ( std::exchange( m_RefusedRoom, room ) == room )
                return;
            LOG_WARN(
                 "[Shadows] Distance Field Sun tile lists needed {} entries (longest {}), the buffer had room "
                 "for {}: that frame's far sun shadow traced the global field only; room for {} from now on",
                 needed, longest, room, m_ListRoom );
        }

        struct Node
        {
            std::shared_ptr<Shader>          Shader;
            std::shared_ptr<ComputePipeline> Pipeline;
            ShaderBindingLayoutCache         Layout;
        };

        [[nodiscard]] static Common::BoolResultStr Make( Node& node, const std::string_view name,
                                                         const std::span<const RDG::ShaderParameter> parameters )
        {
            const std::string shaderName( name );
            node.Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
            if ( !node.Shader )
                return Common::MakeFormattedError<bool>( "Shadows: Distance Field Sun: shader {} not found",
                                                         name );
            if ( const auto valid = node.Layout.Validate( node.Shader, parameters ); !valid )
                return valid;
            const auto pipeline = ComputePipeline::Create( { .Shader = node.Shader, .DebugName = shaderName } );
            if ( !pipeline )
                return Common::MakeError<bool>( pipeline.GetError() );
            node.Pipeline = pipeline.GetValue();
            return BOOLSUCCESS;
        }

        Node m_Cull;
        Node m_Offsets;
        Node m_Trace;
        Node m_Upsample;

        // The tile list entries the buffer has room for after the headers, grown, never cut (ReadFeedback).
        uint64_t m_ListRoom = kDistanceFieldShadowInitialListRoom;
        // Per frame slot: the HostRead buffer the offsets pass extracted into and that cull's stamp (0 = none).
        // Sized once per frames-in-flight count, never per frame.
        std::vector<RDG::ExternalBuffer> m_Feedback;
        std::vector<uint32_t>            m_FeedbackStamp;
        uint32_t                         m_Stamp          = 0;
        uint32_t                         m_RefusedRoom    = 0;     // the room of the last refusal logged
        bool                             m_FeedbackLogged = false; // the first readback is logged once
    };
} // namespace Desert::Graphic::System

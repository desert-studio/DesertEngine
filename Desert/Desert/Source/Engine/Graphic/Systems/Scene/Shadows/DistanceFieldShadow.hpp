#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
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
#include <memory>
#include <span>
#include <string>
#include <string_view>

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
    // UE GAverageObjectsPerShadowCullTile (128): the list entries' room per tile. The tile buffer is sized from it
    // once (a constant size, every frame); a frame whose lists need more is refused whole on the GPU
    // (DistanceFieldShadowCullOffsets.shader: every list empty) - never cut short.
    inline constexpr uint32_t kDistanceFieldShadowObjectsPerTile = 128;
    inline constexpr uint32_t kDistanceFieldShadowListRoom =
         kDistanceFieldShadowTiles * kDistanceFieldShadowObjectsPerTile;
    // Common/DistanceFieldShadow.glslh DistanceFieldShadowHeaderStride: per tile its list's first index, length.
    inline constexpr uint32_t kDistanceFieldShadowHeaderStride = 2;
    inline constexpr uint32_t kDistanceFieldShadowTileUints =
         kDistanceFieldShadowTiles * kDistanceFieldShadowHeaderStride + kDistanceFieldShadowListRoom;
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
        glm::uvec4 Counts{ 0u };                     // mesh objects, tiles a side, tiles, list room
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
        static constexpr std::array kAll{ RDG::Erase( DistanceFieldShadowParamsUB ),
                                          RDG::Erase( DistanceFieldShadowTilesUB ) };
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
    // lowest receiver is the lowest of those points along the light's z.
    [[nodiscard]] inline DistanceFieldShadowGpuParams
    MakeDistanceFieldShadowGpuParams( const DistanceFieldShadowInputs& in )
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
        gpu.Counts            = glm::uvec4( in.Fields.NumMeshObjects, kDistanceFieldShadowTilesPerSide,
                                            kDistanceFieldShadowTiles, kDistanceFieldShadowListRoom );
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

            const DistanceFieldShadowGpuParams gpu = MakeDistanceFieldShadowGpuParams( in );
            const RDG::BufferRef               params =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( gpu ) }, "ShadowMask.Sun.DistanceField.Params" );
            graph.QueueBufferUpload( params, std::as_bytes( std::span( &gpu, 1 ) ) );
            const RDG::BufferRef tiles =
                 graph.CreateBuffer( RDG::BufferDesc{ uint64_t{ kDistanceFieldShadowTileUints } * 4ull },
                                     "ShadowMask.Sun.DistanceField.Tiles" );
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

            // 2: the counts' prefix sum into the lists' first indices (one workgroup).
            {
                ComputePipeline* const pipeline = m_Offsets.Pipeline.get();
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
                                        tiles, kW );
                     },
                     [pipeline]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "Shadows: Distance Field Sun: Cull Offsets" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, 1u, 1u, 1u );
                     } );
            }
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
    };
} // namespace Desert::Graphic::System

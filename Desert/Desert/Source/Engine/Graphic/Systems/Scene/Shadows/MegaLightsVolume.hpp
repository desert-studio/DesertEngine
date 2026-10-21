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
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>

// THE MEGALIGHTS TRANSLUCENCY VOLUME (UE 5.5 MegaLights' volume + TranslucencyLightingVolume; Shadows Stochastic):
// the surfaces that have no G-buffer pixel - the forward meshes and the translucent ones - take their local lights
// from a volume MegaLights lit, shadowed, instead of looping the lights of their froxel unshadowed. The view's
// frustum is cut into froxels (Shadows.MegaLightsVolumeTilePixels square tiles x Shadows.MegaLightsVolumeSlices
// logarithmic slices between the light grid's near and far); per froxel, per frame, one compute pass "MegaLights:
// Volume" (Programs/Shadows/MegaLightsVolume.shader) draws Shadows.MegaLightsVolumeSamples local lights by
// resampled importance over the light-grid cell, traces each once toward its source sphere, projects the shadowed
// estimate on L1 spherical harmonics (one RGBA16F volume per colour channel) and blends it with the previous
// frame's volume at the reprojected froxel (Shadows.MegaLightsMaxHistory, as the surface samples' temporal reuse).
// The readers: Common/MegaLightsVolume.glslh (Mesh/Surface/Pass_Forward.glslh, Pass_Forward_Translucent.glslh),
// bound by name through SceneViewInputs::MegaLightsVolume.
// The history is this object's, per view: two slots of three volumes, one read and one written per frame, created
// once at a size and extracted, as MegaLightsRenderer's.
namespace Desert::Graphic::System
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MegaLightsVolumeShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, MegaLightsVolumeViewUB );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( AccelerationStructure, SceneTlas );
        DESERT_SHADER_PARAMETER( StorageBuffer, RayTracingGeometryRecords );
        DESERT_SHADER_PARAMETER( StorageBuffer, RayTracingMaterialRecords );
    };

    // The most traced samples a froxel takes per frame: the shader's MEGALIGHTS_VOLUME_MAX_SAMPLES
    // (Scalability 1..4).
    inline constexpr uint32_t kMegaLightsVolumeMaxSamples = 4;
    // UE r.RayTracing.NormalBias (0.1 cm), as the surface samples' rays: the gap a froxel ray leaves at both ends.
    inline constexpr float kMegaLightsVolumeRayBias = 0.1f;

    // Scalability Shadows.MegaLightsVolume* (and the shared Shadows.MegaLightsMaxHistory), taken each BeginScene.
    struct MegaLightsVolumeQuality
    {
        uint32_t TilePixels = 16; // froxel edge, render pixels
        uint32_t Slices     = 64;
        uint32_t Samples    = 1; // 1..kMegaLightsVolumeMaxSamples
        uint32_t MaxHistory = 0; // frames; 0 = no temporal accumulation
    };

    // This frame's inputs, graph refs of this view's graph.
    struct MegaLightsVolumeInputs
    {
        RDG::BufferRef                      Tlas;
        RayTracing::RayTracingMetadataFrame Metadata;
        ViewLightBuffers                    Lights;  // the rows and the light grid (AddFrameLightGrid)
        RDG::TextureRef                     Black3D; // System.Black3D: the history before one exists
        uint32_t                            PointLights = 0;
        uint32_t                            SpotLights  = 0;
        uint32_t                            Width       = 0; // render resolution
        uint32_t                            Height      = 0;
        // The light grid's depth range (LightGridParamsPayload::ZParams x = near, z = far, cm): the volume slices
        // the same range, so every froxel lies where some light can reach.
        float Near = 1.0f;
        float Far  = 2.0f;
    };

    // MegaLightsVolumeViewUB (Programs/Shadows/MegaLightsVolume.shader), std140.
    struct MegaLightsVolumeUniforms
    {
        glm::mat4  InvViewProjection;
        glm::mat4  View;
        glm::mat4  PrevView;
        glm::mat4  PrevViewProjection;
        glm::vec4  ZParams;     // near, slices / log(far / near), far, froxel edge (px)
        glm::vec4  PrevZParams; // the same, of the frame the history was written in
        glm::uvec4 Size;        // froxels xyz, frame index
        glm::uvec4 Counts;      // points, spots, samples per froxel, history readable
        glm::vec4  Params;      // history weight, ray bias (cm), render extent (px)
    };
    static_assert( sizeof( MegaLightsVolumeUniforms ) == 336 );

    // MegaLightsVolumeUB (Common/MegaLightsVolume.glslh), std430: what a reader maps a fragment with.
    struct MegaLightsVolumeReadParams
    {
        glm::mat4  View;
        glm::vec4  ZParams;
        glm::uvec4 Size; // froxels xyz, on (1) / off (0)
    };
    static_assert( sizeof( MegaLightsVolumeReadParams ) == 96 );

    // The volume OFF for this graph: a block whose Size.w is 0 and System.Black3D in the three slots. The forward
    // cell then loops its light grid, the glass adds no local light.
    [[nodiscard]] inline MegaLightsVolumeRefs AddMegaLightsVolumeOff( RDG::Builder&         graph,
                                                                      const RDG::TextureRef black3D )
    {
        const MegaLightsVolumeReadParams off{
             .View = glm::mat4( 1.0f ), .ZParams = glm::vec4( 1.0f ), .Size = glm::uvec4( 1u, 1u, 1u, 0u ) };
        const RDG::BufferRef params = graph.CreateBuffer( RDG::BufferDesc{ sizeof( MegaLightsVolumeReadParams ) },
                                                          "MegaLights.Volume.Params" );
        graph.QueueBufferUpload( params, std::as_bytes( std::span<const MegaLightsVolumeReadParams>( &off, 1 ) ) );
        return MegaLightsVolumeRefs{ .Params = params, .Lighting = { black3D, black3D, black3D } };
    }

    class MegaLightsVolumeRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( std::string( kShaderName ) );
            if ( !m_Shader )
                return Common::MakeError( std::format( "MegaLights: shader {} not found", kShaderName ) );
            const auto pipeline =
                 ComputePipeline::Create( { .Shader = m_Shader, .DebugName = std::string( kShaderName ) } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            return BOOLSUCCESS;
        }

        // Declares "MegaLights: Volume" and returns the volume ON. An error, never an unshadowed volume, when the
        // frame has no TLAS or no light grid.
        [[nodiscard]] Common::ResultStr<MegaLightsVolumeRefs> AddPasses( RDG::Builder&                  graph,
                                                                         const MegaLightsVolumeInputs&  in,
                                                                         const MegaLightsVolumeQuality& quality,
                                                                         const ViewFrame&               frame )
        {
            if ( !m_Pipeline )
                return Common::MakeError<MegaLightsVolumeRefs>(
                     "MegaLights: Volume: the pipeline is not initialised" );
            if ( !in.Tlas.IsValid() || !in.Metadata.Geometry.IsValid() || !in.Metadata.Materials.IsValid() )
                return Common::MakeError<MegaLightsVolumeRefs>(
                     "MegaLights: Volume: Shadows resolved to Stochastic but the frame has no TLAS" );
            if ( !in.Lights.GridParams.IsValid() || !in.Lights.GridCells.IsValid() || !in.Black3D.IsValid() )
                return Common::MakeError<MegaLightsVolumeRefs>( std::format(
                     "MegaLights: Volume: the frame has no {}",
                     in.Black3D.IsValid() ? "light grid" : "System.Black3D to stand for the history" ) );
            if ( in.Width == 0 || in.Height == 0 || !( in.Far > in.Near ) || !( in.Near > 0.0f ) )
                return Common::MakeError<MegaLightsVolumeRefs>(
                     std::format( "MegaLights: Volume: no froxel of a {}x{} view between {} and {} cm", in.Width,
                                  in.Height, in.Near, in.Far ) );

            const uint32_t      tile   = std::max( quality.TilePixels, 1u );
            const uint32_t      slices = std::max( quality.Slices, 1u );
            const RDG::Extent3D size{ .Width  = ( in.Width + tile - 1u ) / tile,
                                      .Height = ( in.Height + tile - 1u ) / tile,
                                      .Depth  = slices };
            const glm::vec4 zParams( in.Near, static_cast<float>( slices ) / std::log( in.Far / in.Near ), in.Far,
                                     static_cast<float>( tile ) );

            // The history: readable when the previous frame of this view wrote it at this size.
            HistorySlot& previous = m_Slots[m_Parity];
            HistorySlot& next     = m_Slots[1u - m_Parity];
            const bool   readable = quality.MaxHistory > 0 && previous.Built && previous.Size == size &&
                                  previous.Stamp.ReadableIn( frame );
            std::array<RDG::TextureRef, 3> history = { in.Black3D, in.Black3D, in.Black3D };
            if ( readable )
                history = AcquireSlot( graph, previous, m_Parity, size );
            const glm::vec4                      prevZParams = readable ? previous.ZParams : zParams;
            const std::array<RDG::TextureRef, 3> written     = AcquireSlot( graph, next, 1u - m_Parity, size );
            next.ZParams                                     = zParams;
            m_Parity                                         = 1u - m_Parity;

            const MegaLightsVolumeUniforms uniforms{
                 .InvViewProjection  = frame.InvViewProjection,
                 .View               = frame.View,
                 .PrevView           = frame.PrevView,
                 .PrevViewProjection = frame.PrevViewProjection,
                 .ZParams            = zParams,
                 .PrevZParams        = prevZParams,
                 .Size =
                      glm::uvec4( size.Width, size.Height, size.Depth, static_cast<uint32_t>( frame.FrameIndex ) ),
                 .Counts = glm::uvec4( in.PointLights, in.SpotLights,
                                       std::clamp( quality.Samples, 1u, kMegaLightsVolumeMaxSamples ),
                                       readable ? 1u : 0u ),
                 .Params =
                      glm::vec4( 1.0f / static_cast<float>( quality.MaxHistory + 1u ), kMegaLightsVolumeRayBias,
                                 static_cast<float>( in.Width ), static_cast<float>( in.Height ) ) };
            const RDG::BufferRef view = graph.CreateBuffer( RDG::BufferDesc{ sizeof( MegaLightsVolumeUniforms ) },
                                                            "MegaLights.Volume.ViewUB" );
            graph.QueueBufferUpload( view,
                                     std::as_bytes( std::span<const MegaLightsVolumeUniforms>( &uniforms, 1 ) ) );

            const MegaLightsVolumeReadParams readParams{
                 .View    = frame.View,
                 .ZParams = zParams,
                 .Size    = glm::uvec4( size.Width, size.Height, size.Depth, 1u ) };
            const RDG::BufferRef params = graph.CreateBuffer(
                 RDG::BufferDesc{ sizeof( MegaLightsVolumeReadParams ) }, "MegaLights.Volume.Params" );
            graph.QueueBufferUpload(
                 params, std::as_bytes( std::span<const MegaLightsVolumeReadParams>( &readParams, 1 ) ) );

            HistorySlot* stamped = &next;
            graph.AddPass(
                 kPassName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     auto block =
                          pass.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) );
                     block.Uniform( MegaLightsVolumeShaderParameters::MegaLightsVolumeViewUB, view )
                          .Sampler( MegaLightsVolumeShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( ShaderProtocols::PointLight::Name, in.Lights.Point, RDG::Access::StorageRead )
                          .Storage( ShaderProtocols::SpotLight::Name, in.Lights.Spot, RDG::Access::StorageRead )
                          .Storage( kLightGridParamsBlockName, in.Lights.GridParams, RDG::Access::StorageRead )
                          .Storage( kLightGridCellsBlockName, in.Lights.GridCells, RDG::Access::StorageRead )
                          .AccelerationStructure( MegaLightsVolumeShaderParameters::SceneTlas, in.Tlas )
                          .Storage( MegaLightsVolumeShaderParameters::RayTracingGeometryRecords,
                                    in.Metadata.Geometry, RDG::Access::StorageRead )
                          .Storage( MegaLightsVolumeShaderParameters::RayTracingMaterialRecords,
                                    in.Metadata.Materials, RDG::Access::StorageRead );
                     for ( std::size_t channel = 0; channel < 3; ++channel )
                         block.Texture( kHistoryInputNames[channel], history[channel], RDG::Access::SampledCompute,
                                        RDG::SubresourceRange::All() )
                              .Storage( kOutputNames[channel], written[channel], RDG::Access::StorageWrite );
                 },
                 [this, size, stamped,
                  index = frame.FrameIndex]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "MegaLights" );
                     const RDG::PassBindings     bindings( context, context.GetBindingBlock( 0 ) );
                     const Common::BoolResultStr dispatched = Renderer::GetInstance().DispatchCompute(
                          bindings, *m_Pipeline, ( size.Width + 3u ) / 4u, ( size.Height + 3u ) / 4u,
                          ( size.Depth + 3u ) / 4u );
                     if ( dispatched.IsSuccess() )
                         stamped->Stamp.Stamp( index );
                     return dispatched;
                 } );
            return Common::MakeSuccess( MegaLightsVolumeRefs{ .Params = params, .Lighting = written } );
        }

    private:
        static constexpr std::string_view                kShaderName        = "MegaLightsVolume";
        static constexpr std::string_view                kPassName          = "MegaLights: Volume";
        static constexpr std::array<std::string_view, 3> kHistoryInputNames = { "HistoryRTexture", "HistoryGTexture",
                                                                                "HistoryBTexture" };
        static constexpr std::array<std::string_view, 3> kOutputNames       = { "OutRTexture", "OutGTexture", "OutBTexture" };
        // The two history slots' graph names (static: the graph keeps a view of each while it builds).
        static constexpr std::array<std::array<std::string_view, 3>, 2> kSlotNames = {
             { { "MegaLights.Volume0.R", "MegaLights.Volume0.G", "MegaLights.Volume0.B" },
               { "MegaLights.Volume1.R", "MegaLights.Volume1.G", "MegaLights.Volume1.B" } } };

        // One history slot: the three volumes, and the depth range they were sliced with (the far plane follows
        // the lights, so last frame's slices are not this frame's).
        struct HistorySlot
        {
            std::array<RDG::ExternalTexture, 3> Lighting;
            bool                                Built = false;
            RDG::Extent3D                       Size;
            glm::vec4                           ZParams{ 0.0f };
            PassHistoryStamp                    Stamp; // which view frame wrote it
        };

        // The slot's three volumes in this graph: registered when they exist at this size, created and extracted
        // (kept past the graph: next frame's history and this frame's readers) when they do not.
        static std::array<RDG::TextureRef, 3> AcquireSlot( RDG::Builder& graph, HistorySlot& slot,
                                                           const uint32_t index, const RDG::Extent3D& size )
        {
            std::array<RDG::TextureRef, 3> refs;
            if ( slot.Built && slot.Size == size )
            {
                for ( std::size_t channel = 0; channel < 3; ++channel )
                    refs[channel] = graph.RegisterExternal( slot.Lighting[channel], kSlotNames[index][channel] );
                return refs;
            }
            slot.Stamp.Invalidate();
            for ( std::size_t channel = 0; channel < 3; ++channel )
            {
                slot.Lighting[channel] = {};
                refs[channel] =
                     graph.CreateTexture( RDG::TextureDesc{ .Size   = size,
                                                            .Format = Core::Formats::ImageFormat::RGBA16F,
                                                            .Dim    = RDG::TextureDim::Tex3D },
                                          kSlotNames[index][channel] );
                graph.Extract( refs[channel], slot.Lighting[channel], RDG::Access::SampledCompute );
            }
            slot.Built = true;
            slot.Size  = size;
            return refs;
        }

        std::shared_ptr<Shader>          m_Shader;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache m_Layout;
        std::array<HistorySlot, 2>       m_Slots;
        uint32_t                         m_Parity = 0;
    };
} // namespace Desert::Graphic::System

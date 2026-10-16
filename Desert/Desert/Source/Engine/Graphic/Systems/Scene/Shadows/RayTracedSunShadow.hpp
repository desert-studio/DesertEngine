#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

// "Shadows: Ray Traced Sun" (UE RayTracingShadows.cpp + FScreenShadowMaskTexture): ShadowMethod::HardwareTraced
// for the sun. Every G-buffer pixel traces ray-query visibility over the view's TLAS
// (Common/RayQueryVisibility.glslh, alpha-testing masked segments through the bindless table) inside the sun's
// SourceAngle cone, into a full-resolution screen shadow mask + closest-occluder distance; the DENOISE-CORE
// ShadowVisibility signal denoises it when the angle is > 0, and DeferredLighting reads the mask in place of the
// cascades.
namespace Desert::Graphic::System
{
    inline constexpr std::string_view kRayTracedSunShadowPassName = "Shadows: Ray Traced Sun";
    inline constexpr std::string_view kShadowMaskSunName          = "ShadowMask.Sun";
    inline constexpr std::string_view kShadowMaskSunOccluderName  = "ShadowMask.Sun.Occluder";

    // UE r.RayTracing.NormalBias (0.1 cm): a constant of the method, not a setting.
    inline constexpr float kRayTracedShadowNormalBias = 0.1f;
    // The sun is at infinity: its shadow ray is unbounded, every instance of the TLAS may occlude (UE traces a
    // directional light's ray to 1e27).
    inline constexpr float kRayTracedSunShadowMaxDistance = 1.0e27f;

    // Per view, per frame (Docs/RENDERER_FRAME_STATE.md).
    struct RayTracedSunShadowParams
    {
        glm::vec3 DirectionToSun{ 0.0f, 1.0f, 0.0f };
        float     SourceAngleRadians = 0.0f; // DirectionalLightData::SourceAngle; 0 = hard, 1 ray, no denoise
        float     NormalBias         = kRayTracedShadowNormalBias;
        uint32_t  SamplesPerPixel    = 1;    // Scalability Shadows.RayTracedSamplesPerPixel
        float     MaxDistance        = 0.0f; // cm
        uint32_t  FrameIndex         = 0;    // seeds the cone sampling
    };

    // @p sourceAngleDegrees: DirectionalLightData::SourceAngle. @p samplesPerPixel: the resolved Scalability row.
    [[nodiscard]] inline RayTracedSunShadowParams
    MakeRayTracedSunShadowParams( const float sourceAngleDegrees, const glm::vec3& directionToSun,
                                  const uint32_t samplesPerPixel, const float maxDistance, const uint32_t frame )
    {
        RayTracedSunShadowParams params;
        params.DirectionToSun     = glm::normalize( directionToSun );
        params.SourceAngleRadians = glm::radians( sourceAngleDegrees > 0.0f ? sourceAngleDegrees : 0.0f );
        params.SamplesPerPixel    = params.SourceAngleRadians > 0.0f && samplesPerPixel > 0 ? samplesPerPixel : 1u;
        params.MaxDistance        = maxDistance;
        params.FrameIndex         = frame;
        return params;
    }

    struct ScreenShadowMaskInputs
    {
        RDG::TextureRef                     Depth;
        RDG::TextureRef                     Normal;
        RDG::BufferRef                      Tlas;
        RayTracing::RayTracingMetadataFrame Metadata;
        glm::mat4                           InvJitteredViewProjection{ 1.0f };
        uint32_t                            Width  = 0;
        uint32_t                            Height = 0;
    };

    struct RayTracedSunShadowOutputs
    {
        RDG::TextureRef Mask;            // r = visibility (1 = lit)
        RDG::TextureRef ClosestOccluder; // r = hit distance cm, -1 = none
    };

    // The push block of RayTracedSunShadow.shader (96 B).
    struct RayTracedSunShadowPush
    {
        glm::mat4 InvJitteredViewProjection;
        glm::vec4 DirectionToSun; // xyz, w = SourceAngle radians
        glm::vec4 TraceParams;    // normal bias, samples, max distance, frame
    };
    static_assert( sizeof( RayTracedSunShadowPush ) == 96 );

    class RayTracedSunShadowRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "RayTracedSunShadow" );
            if ( !m_Shader )
                return Common::MakeError( "Shadows: Ray Traced Sun: shader RayTracedSunShadow not found" );
            const auto pipeline =
                 ComputePipeline::Create( { .Shader = m_Shader, .DebugName = "RayTracedSunShadow" } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            return BOOLSUCCESS;
        }

        // Declares "Shadows: Ray Traced Sun". An error, never a silent CSM, when the frame's TLAS was not built.
        [[nodiscard]] Common::ResultStr<RayTracedSunShadowOutputs>
        AddPass( RDG::Builder& graph, const ScreenShadowMaskInputs& in, const RayTracedSunShadowParams& params )
        {
            if ( !m_Pipeline )
                return Common::MakeError<RayTracedSunShadowOutputs>(
                     "Shadows: Ray Traced Sun: the pipeline is not initialised" );
            if ( !in.Tlas.IsValid() || !in.Metadata.Geometry.IsValid() || !in.Metadata.Materials.IsValid() )
                return Common::MakeError<RayTracedSunShadowOutputs>(
                     "Shadows: Ray Traced Sun: Shadows resolved to HardwareTraced but the frame has no TLAS" );
            RayTracedSunShadowOutputs out;
            const RDG::TextureDesc    desc{ .Size   = { .Width = in.Width, .Height = in.Height },
                                            .Format = ViewTargetFormats::kDenoiserSignal };
            out.Mask            = graph.CreateTexture( desc, kShadowMaskSunName );
            out.ClosestOccluder = graph.CreateTexture( desc, kShadowMaskSunOccluderName );
            graph.SetFaultDefault( out.Mask, RDG::FaultDefault::White );
            graph.SetFaultDefault( out.ClosestOccluder, RDG::FaultDefault::Black );
            const RayTracedSunShadowPush push{
                 .InvJitteredViewProjection = in.InvJitteredViewProjection,
                 .DirectionToSun            = glm::vec4( params.DirectionToSun, params.SourceAngleRadians ),
                 .TraceParams = glm::vec4( params.NormalBias, static_cast<float>( params.SamplesPerPixel ),
                                           params.MaxDistance, static_cast<float>( params.FrameIndex ) ) };
            const uint32_t width  = in.Width;
            const uint32_t height = in.Height;
            graph.AddPass(
                 kRayTracedSunShadowPassName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                          .AccelerationStructure( "u_SceneTLAS", in.Tlas )
                          .Sampled( "u_GBufferDepth", in.Depth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_GBufferNormal", in.Normal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                          .Storage( "RayTracingGeometryRecords", in.Metadata.Geometry, RDG::Access::StorageRead )
                          .Storage( "RayTracingMaterialRecords", in.Metadata.Materials, RDG::Access::StorageRead )
                          .Storage( "u_ShadowMask", out.Mask, RDG::Access::StorageWrite )
                          .Storage( "u_ShadowOccluder", out.ClosestOccluder, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( RayTracedSunShadowPush ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Shadows: Ray Traced Sun" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, ( width + 7u ) / 8u,
                                                                     ( height + 7u ) / 8u, 1 );
                 } );
            return Common::MakeSuccess( out );
        }

    private:
        std::shared_ptr<Shader>          m_Shader;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache m_Layout;
    };
} // namespace Desert::Graphic::System

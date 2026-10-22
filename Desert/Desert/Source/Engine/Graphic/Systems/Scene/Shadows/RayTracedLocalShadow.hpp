#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/RayTracedSunShadow.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>

// "Shadows: Ray Traced Local" (UE RayTracingShadows.cpp, per light): ShadowMethod::HardwareTraced for the point
// and spot lights. UE renders every shadowed local light alone (its screen shadow mask, then RenderLight); here
// the composite shades every light in one light-grid loop, so each shadowed light of the frame gets a SLOT: one
// trace pass per slot over the G-buffer (SamplesPerPixel rays at the light's emitter sphere,
// Common/LightSourceSampling. glslh - the sampler MegaLights' visibility uses), its ShadowVisibility denoise
// (history per light, SceneRenderer), then "Shadows: Ray Traced Local Pack" copies it into layer <slot> of one
// 2D-array mask the composite's DESERT_LOCAL_SHADOW_SCREEN_MASK program reads (Mesh/LocalLightShadow.glslh, light
// payload ShadowMaskSlot).
namespace Desert::Graphic::System
{
    inline constexpr std::string_view kRayTracedLocalShadowPassName = "Shadows: Ray Traced Local";
    inline constexpr std::string_view kRayTracedLocalShadowPackName = "Shadows: Ray Traced Local Pack";
    inline constexpr std::string_view kShadowMaskLocalName          = "ShadowMask.Local";
    inline constexpr std::string_view kShadowMaskLocalOccluderName  = "ShadowMask.Local.Occluder";
    inline constexpr std::string_view kShadowMaskLocalLayersName    = "ShadowMask.LocalLayers";
    // A point light has no cone: the trace's cone test (dot >= cosOuter) passes everywhere.
    inline constexpr float kRayTracedLocalShadowNoCone = -2.0f;

    // One slot of the frame (SceneRenderer::AssignLocalShadowMaskSlots), from the light's payload.
    struct RayTracedLocalShadowLight
    {
        glm::vec3 Position{ 0.0f };         // cm
        float     AttenuationRadius = 0.0f; // cm: PointLightPayload::Radius / SpotLightPayload::Range
        float     SourceRadius      = 0.0f; // cm: UE SourceRadius; 0 = a point, a hard shadow, no denoise
        glm::vec3 SpotDirection{ 0.0f, 0.0f, -1.0f }; // out of the cone apex
        float     CosOuter = kRayTracedLocalShadowNoCone;
        uint32_t  SourceId = 0; // LocalLightShadowSettings::SourceId: the denoiser's history follows it
    };

    // The push block of RayTracedLocalShadow.shader (112 B).
    struct RayTracedLocalShadowPush
    {
        glm::mat4 InvJitteredViewProjection;
        glm::vec4 Light;       // xyz position cm, w attenuation radius cm
        glm::vec4 TraceParams; // source radius cm, normal bias cm, samples, frame
        glm::vec4 Spot;        // xyz direction out of the apex, w cos outer (kRayTracedLocalShadowNoCone = point)
    };
    static_assert( sizeof( RayTracedLocalShadowPush ) == 112 );

    // The push block of RayTracedLocalShadowPack.shader (16 B).
    struct RayTracedLocalShadowPackPush
    {
        glm::ivec4 Layer; // x = the slot's layer
    };
    static_assert( sizeof( RayTracedLocalShadowPackPush ) == 16 );

    class RayTracedLocalShadowRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
            m_TraceShader       = shaders->GetByName( "RayTracedLocalShadow" );
            m_PackShader        = shaders->GetByName( "RayTracedLocalShadowPack" );
            if ( !m_TraceShader || !m_PackShader )
                return Common::MakeError( "Shadows: Ray Traced Local: shader RayTracedLocalShadow or "
                                          "RayTracedLocalShadowPack not found" );
            const auto trace =
                 ComputePipeline::Create( { .Shader = m_TraceShader, .DebugName = "RayTracedLocalShadow" } );
            if ( !trace )
                return Common::MakeError( trace.GetError() );
            const auto pack =
                 ComputePipeline::Create( { .Shader = m_PackShader, .DebugName = "RayTracedLocalShadowPack" } );
            if ( !pack )
                return Common::MakeError( pack.GetError() );
            m_TracePipeline = trace.GetValue();
            m_PackPipeline  = pack.GetValue();
            return BOOLSUCCESS;
        }

        // The frame's 2D-array mask: one layer per slot, at least one (a frame with no slot packs System.White
        // into layer 0, so the composite never reads an unwritten layer).
        [[nodiscard]] static RDG::TextureRef CreateMasks( RDG::Builder& graph, const uint32_t width,
                                                          const uint32_t height, const uint32_t slots )
        {
            const RDG::TextureRef masks =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = ViewTargetFormats::kDenoiserSignal,
                                                        .Layers = std::max( slots, 1u ),
                                                        .Dim    = RDG::TextureDim::Tex2DArray },
                                      kShadowMaskLocalLayersName );
            graph.SetFaultDefault( masks, RDG::FaultDefault::White );
            return masks;
        }

        // Declares "Shadows: Ray Traced Local" for one slot. An error, never an unshadowed light in silence, when
        // the frame's TLAS was not built.
        [[nodiscard]] Common::ResultStr<RayTracedSunShadowOutputs>
        AddTracePass( RDG::Builder& graph, const ScreenShadowMaskInputs& in,
                      const RayTracedLocalShadowLight& light, const uint32_t samplesPerPixel,
                      const uint32_t frameIndex )
        {
            if ( !m_TracePipeline )
                return Common::MakeError<RayTracedSunShadowOutputs>(
                     "Shadows: Ray Traced Local: the pipeline is not initialised" );
            if ( !in.Tlas.IsValid() || !in.Metadata.Geometry.IsValid() || !in.Metadata.Materials.IsValid() )
                return Common::MakeError<RayTracedSunShadowOutputs>(
                     "Shadows: Ray Traced Local: Shadows resolved to HardwareTraced but the frame has no TLAS" );
            RayTracedSunShadowOutputs out;
            const RDG::TextureDesc    desc{ .Size   = { .Width = in.Width, .Height = in.Height },
                                            .Format = ViewTargetFormats::kDenoiserSignal };
            out.Mask            = graph.CreateTexture( desc, kShadowMaskLocalName );
            out.ClosestOccluder = graph.CreateTexture( desc, kShadowMaskLocalOccluderName );
            graph.SetFaultDefault( out.Mask, RDG::FaultDefault::White );
            graph.SetFaultDefault( out.ClosestOccluder, RDG::FaultDefault::Black );
            // A point emitter casts a hard shadow: one ray says all (as the sun of angle 0).
            const uint32_t samples = light.SourceRadius > 0.0f && samplesPerPixel > 0 ? samplesPerPixel : 1u;
            const RayTracedLocalShadowPush push{
                 .InvJitteredViewProjection = in.InvJitteredViewProjection,
                 .Light                     = glm::vec4( light.Position, light.AttenuationRadius ),
                 .TraceParams               = glm::vec4( light.SourceRadius, kRayTracedShadowNormalBias,
                                                         static_cast<float>( samples ), static_cast<float>( frameIndex ) ),
                 .Spot                      = glm::vec4( light.SpotDirection, light.CosOuter ) };
            const uint32_t width  = in.Width;
            const uint32_t height = in.Height;
            graph.AddPass(
                 kRayTracedLocalShadowPassName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_TraceLayout.Get( m_TraceShader ),
                                    Renderer::GetPipelineRouteFill( *m_TracePipeline ) )
                          .AccelerationStructure( "u_SceneTLAS", in.Tlas )
                          .Texture( "u_GBufferDepth", in.Depth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_GBufferNormal", in.Normal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Storage( "RayTracingGeometryRecords", in.Metadata.Geometry, RDG::Access::StorageRead )
                          .Storage( "RayTracingMaterialRecords", in.Metadata.Materials, RDG::Access::StorageRead )
                          .Storage( "u_ShadowMask", out.Mask, RDG::Access::StorageWrite )
                          .Storage( "u_ShadowOccluder", out.ClosestOccluder, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( RayTracedLocalShadowPush ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Shadows: Ray Traced Local" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute(
                          bindings, *m_TracePipeline, ( width + 7u ) / 8u, ( height + 7u ) / 8u, 1 );
                 } );
            return Common::MakeSuccess( out );
        }

        // Declares "Shadows: Ray Traced Local Pack": @p visibility (r; any extent - System.White is 1x1, it is
        // read at the pixel's uv) into layer @p layer of @p masks. Every slot's pack writes the same array, each
        // its own layer: the graph orders them in declaration order. An error when not initialised.
        [[nodiscard]] Common::BoolResultStr AddPackPass( RDG::Builder& graph, const RDG::TextureRef visibility,
                                                         const RDG::TextureRef masks, const uint32_t layer,
                                                         const uint32_t width, const uint32_t height )
        {
            if ( !m_PackPipeline )
                return Common::MakeError( "Shadows: Ray Traced Local Pack: the pipeline is not initialised" );
            const RayTracedLocalShadowPackPush push{ .Layer =
                                                          glm::ivec4( static_cast<int32_t>( layer ), 0, 0, 0 ) };
            graph.AddPass(
                 kRayTracedLocalShadowPackName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_PackLayout.Get( m_PackShader ),
                                    Renderer::GetPipelineRouteFill( *m_PackPipeline ) )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_Visibility", visibility, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Storage( "u_LocalShadowMasks", masks, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( RayTracedLocalShadowPackPush ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Shadows: Ray Traced Local Pack" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute(
                          bindings, *m_PackPipeline, ( width + 7u ) / 8u, ( height + 7u ) / 8u, 1 );
                 } );
            return BOOLSUCCESS;
        }

    private:
        std::shared_ptr<Shader>          m_TraceShader;
        std::shared_ptr<Shader>          m_PackShader;
        std::shared_ptr<ComputePipeline> m_TracePipeline;
        std::shared_ptr<ComputePipeline> m_PackPipeline;
        mutable ShaderBindingLayoutCache m_TraceLayout;
        mutable ShaderBindingLayoutCache m_PackLayout;
    };
} // namespace Desert::Graphic::System

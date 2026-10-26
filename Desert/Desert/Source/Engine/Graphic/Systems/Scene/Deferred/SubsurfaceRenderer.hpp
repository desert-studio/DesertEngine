#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/SubsurfaceProfiles/SubsurfaceProfileRegistry.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <format>
#include <span>

namespace Desert::Graphic::System
{
    // The slot names "Subsurface: Burley" binds, spelled once (RDGShaderParameters.hpp).
    struct SubsurfaceBurleyShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, SubsurfaceDiffuseTexture );
        DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, SubsurfaceProfileAtlasUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SubsurfaceViewUB );
        DESERT_SHADER_PARAMETER( StorageTexture, SubsurfaceScatteredTexture );
    };

    // The slot names "Subsurface: Recombine" binds.
    struct SubsurfaceRecombineShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, SubsurfaceDiffuseTexture );
        DESERT_SHADER_PARAMETER( Texture, SubsurfaceScatteredTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, SubsurfaceProfileAtlasUB );
    };

    // SubsurfaceBurley.shader's SubsurfaceViewUB (std430).
    struct SubsurfaceViewGpu
    {
        glm::mat4 WorldToClip{ 1.0f };
        glm::mat4 ClipToWorld{ 1.0f };
        glm::vec4 Params{ 0.0f }; // x scale, y samples, z frame index
    };
    static_assert( sizeof( SubsurfaceViewGpu ) == 144 );

    // What the passes read, as refs of THIS frame's graph (CONTRACT §3.5).
    struct SubsurfaceInputs
    {
        RDG::TextureRef SubsurfaceDiffuse; // the composite's split: demodulated diffuse, a = profile row + 1
        RDG::TextureRef GBufferA;          // BaseColor (UE StoredBaseColor)
        RDG::TextureRef GBufferB;          // world normal
        RDG::TextureRef Depth;             // the G-buffer's device depth
    };

    // The view's settings (PostProcessSettings, CONTRACT §6) and its matrices.
    struct SubsurfaceSettings
    {
        float     Scale = 1.0f;
        glm::mat4 JitteredViewProjection{ 1.0f };
        glm::mat4 InvJitteredViewProjection{ 1.0f };
        uint64_t  FrameIndex = 0;
    };

    // The graph refs the Recombine node binds, made by AddScatter.
    struct SubsurfaceScatter
    {
        RDG::BufferRef  Atlas;
        RDG::TextureRef Scattered;
    };

    /**
     * @brief THE SCREEN-SPACE SUBSURFACE SCATTER (SSS-PROFILE), UE AddSubsurfacePass: the profile atlas uploaded
     * by the graph (SubsurfaceProfileRegistry's rows, every frame: 66 vec4 per profile), "Subsurface: Burley"
     * (compute) scatters the composite's SubsurfaceDiffuse with each texel's Burley profile, and "Subsurface:
     * Recombine" (raster, ONE/ONE onto the scene target) adds BaseColor x lerp(diffuse, scattered, Tint) back.
     * Added by SceneRenderer only when the view's SubsurfaceScattering is on and a profile is registered.
     */
    class SubsurfaceRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // AFIS at a fixed count (UE r.SSS.Burley.Quality 0); the variance-adaptive count with history is p5.
        static constexpr uint32_t kBurleySamples = 32;

        Common::BoolResultStr Initialize() override
        {
            auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
            m_BurleyShader      = shaders->GetByName( "SubsurfaceBurley" );
            if ( !m_BurleyShader )
                return Common::MakeError( "SubsurfaceBurley shader not found" );
            const auto burley =
                 ComputePipeline::Create( { .Shader = m_BurleyShader, .DebugName = "SubsurfaceBurley" } );
            if ( !burley )
                return Common::MakeError( burley.GetError() );
            m_BurleyPipeline = burley.GetValue();

            m_RecombineShader = shaders->GetByName( "SubsurfaceRecombine" );
            if ( !m_RecombineShader )
                return Common::MakeError( "SubsurfaceRecombine shader not found" );
            GraphicsPipelineSpecification spec;
            spec.DebugName           = "SubsurfaceRecombine";
            spec.TargetLayout        = SceneTargetLayout();
            spec.Shader              = m_RecombineShader;
            spec.DepthTestEnabled    = false;
            spec.DepthWriteEnabled   = false;
            spec.BlendEnable         = true;
            spec.SrcColorBlendFactor = BlendFactor::One;
            spec.DstColorBlendFactor = BlendFactor::One;
            const auto recombine     = GraphicsPipeline::Create( spec );
            if ( !recombine )
                return Common::MakeError(
                     std::format( "SubsurfaceRecombine pipeline: {}", recombine.GetError() ) );
            m_RecombinePipeline = recombine.GetValue();
            return BOOLSUCCESS;
        }

        [[nodiscard]] bool IsReady() const
        {
            return m_BurleyPipeline && m_RecombinePipeline;
        }

        // GRAPH BUILD: the atlas of @p registry and the view's parameters as graph buffers, then "Subsurface:
        // Burley" into a new @p width x @p height transient. Empty refs when not ready or nothing is registered.
        [[nodiscard]] SubsurfaceScatter AddScatter( RDG::Builder& graph, const SubsurfaceProfileRegistry& registry,
                                                    const SubsurfaceInputs&   inputs,
                                                    const SubsurfaceSettings& settings, const uint32_t width,
                                                    const uint32_t height ) const
        {
            const std::span<const SubsurfaceProfileRow> rows = registry.Rows();
            if ( !IsReady() || rows.empty() || width == 0u || height == 0u )
                return {};
            SubsurfaceScatter scatter;
            scatter.Atlas =
                 graph.CreateBuffer( RDG::BufferDesc{ .Bytes = rows.size_bytes() }, "Subsurface: Profile Atlas" );
            graph.QueueBufferUpload( scatter.Atlas, std::as_bytes( rows ) );

            SubsurfaceViewGpu view;
            view.WorldToClip = settings.JitteredViewProjection;
            view.ClipToWorld = settings.InvJitteredViewProjection;
            view.Params      = glm::vec4( settings.Scale, static_cast<float>( kBurleySamples ),
                                          static_cast<float>( settings.FrameIndex % 1024u ), 0.0f );
            const RDG::BufferRef viewBuffer =
                 graph.CreateBuffer( RDG::BufferDesc{ .Bytes = sizeof( SubsurfaceViewGpu ) }, "Subsurface: View" );
            graph.QueueBufferUpload( viewBuffer, std::as_bytes( std::span( &view, 1 ) ) );

            scatter.Scattered =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = ViewTargetFormats::kSubsurfaceDiffuse },
                                      "SubsurfaceScattered" );
            graph.AddPass(
                 "Subsurface: Burley", RDG::PassFlags::Compute,
                 [&, this]( RDG::PassBuilder& pass )
                 {
                     constexpr RDG::Access kRead = RDG::Access::SampledCompute;
                     const auto            all   = RDG::SubresourceRange::All();
                     pass.Bindings( m_BurleyLayout.Get( m_BurleyShader ),
                                    Renderer::GetPipelineRouteFill( *m_BurleyPipeline ) )
                          .Sampler( SubsurfaceBurleyShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( SubsurfaceBurleyShaderParameters::SubsurfaceDiffuseTexture,
                                    inputs.SubsurfaceDiffuse, kRead, all )
                          .Texture( SubsurfaceBurleyShaderParameters::SceneDepthTexture, inputs.Depth, kRead, all )
                          .Texture( SubsurfaceBurleyShaderParameters::GBufferBTexture, inputs.GBufferB, kRead,
                                    all )
                          .Storage( SubsurfaceBurleyShaderParameters::SubsurfaceProfileAtlasUB, scatter.Atlas,
                                    RDG::Access::StorageRead )
                          .Storage( SubsurfaceBurleyShaderParameters::SubsurfaceViewUB, viewBuffer,
                                    RDG::Access::StorageRead )
                          .Storage( SubsurfaceBurleyShaderParameters::SubsurfaceScatteredTexture,
                                    scatter.Scattered, RDG::Access::StorageWrite );
                 },
                 [pipeline = m_BurleyPipeline, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Subsurface: Burley" );
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, ( width + 7u ) / 8u,
                                                                     ( height + 7u ) / 8u, 1 );
                 } );
            return scatter;
        }

        // SETUP of "Subsurface: Recombine" (the scene target is loaded by the caller's node).
        void DeclareRecombineBindings( RDG::PassBuilder& pass, const SubsurfaceInputs& inputs,
                                       const SubsurfaceScatter& scatter ) const
        {
            if ( !IsReady() )
                return;
            constexpr RDG::Access kRead = RDG::Access::SampledGraphics;
            const auto            all   = RDG::SubresourceRange::All();
            pass.Bindings( m_RecombineLayout.Get( m_RecombineShader ), RDG::OtherRouteFill{} )
                 .Sampler( SubsurfaceRecombineShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( SubsurfaceRecombineShaderParameters::SubsurfaceDiffuseTexture, inputs.SubsurfaceDiffuse,
                           kRead, all )
                 .Texture( SubsurfaceRecombineShaderParameters::SubsurfaceScatteredTexture, scatter.Scattered,
                           kRead, all )
                 .Texture( SubsurfaceRecombineShaderParameters::GBufferATexture, inputs.GBufferA, kRead, all )
                 .Storage( SubsurfaceRecombineShaderParameters::SubsurfaceProfileAtlasUB, scatter.Atlas,
                           RDG::Access::StorageRead );
        }

        [[nodiscard]] Common::BoolResultStr RecordRecombine( const RDG::PassContext& context ) const
        {
            if ( !IsReady() )
                return Common::MakeError( "Subsurface: Recombine recorded without its pipeline" );
            DESERT_PROFILE_PASS( "Subsurface: Recombine" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_RecombinePipeline, nullptr );
        }

    private:
        std::shared_ptr<Shader>           m_BurleyShader;
        mutable ShaderBindingLayoutCache  m_BurleyLayout;
        std::shared_ptr<ComputePipeline>  m_BurleyPipeline;
        std::shared_ptr<Shader>           m_RecombineShader;
        mutable ShaderBindingLayoutCache  m_RecombineLayout;
        std::shared_ptr<GraphicsPipeline> m_RecombinePipeline;
    };
} // namespace Desert::Graphic::System

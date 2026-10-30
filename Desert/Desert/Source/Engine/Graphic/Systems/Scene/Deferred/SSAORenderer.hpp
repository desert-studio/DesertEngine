#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SSAOUniforms.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <span>

namespace Desert::Graphic::System
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SSAOBindingShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferNormalTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, SsaoOutputTexture );
        DESERT_SHADER_PARAMETER( UniformBuffer, SSAOUB );
    };

    // Screen-space ambient occlusion (GTAO, SSAO.shader) as a COMPUTE pass (UE r.AmbientOcclusion.Compute 1, the
    // GTAOCombinedCS): reads the G-buffer (depth + normal) and writes an AO factor into a per-frame graph
    // transient (FrameTransients::SSAO, SceneRendererFrameDeferred.cpp "Deferred: SSAO") as a storage image; the
    // deferred Composite then multiplies it into the ambient term. Being compute with every input a graph
    // resource, the node runs on the async compute pipe (PassFlags::AsyncCompute, UE r.AmbientOcclusion.Compute 2
    // / the async budget) between the G-buffer and the composite. Deferred only. There is no raster path.
    class SSAORenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSAO" );
            if ( !m_Shader )
                return Common::MakeError( "SSAO shader not found" );
            const auto pipeline = ComputePipeline::Create( { .Shader = m_Shader, .DebugName = "SSAO" } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            return BOOLSUCCESS;
        }

        // GRAPH BUILD (before "Deferred: SSAO"): this frame's SSAOUB as a graph buffer, written by the graph's
        // upload pass (Builder::QueueBufferUpload) so the dispatch is ordered after it. Inputs::WorldRadius is a
        // WORLD distance (the shader offsets samples in world space), and a world unit is a centimetre - callers
        // passing literature values must convert through Common::Units.
        static RDG::BufferRef UploadUniforms( RDG::Builder& graph, const SSAOUniforms::Inputs& inputs )
        {
            const SSAOUniforms::SSAOUBData data = SSAOUniforms::Make( inputs );
            const RDG::BufferRef           buffer =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( SSAOUniforms::SSAOUBData ) }, "SSAO.UB" );
            graph.QueueBufferUpload( buffer,
                                     std::as_bytes( std::span<const SSAOUniforms::SSAOUBData>( &data, 1 ) ) );
            return buffer;
        }

        // SETUP: declares the node's one block (block 0): GBufferDepthTexture through a nearest sampler (a
        // DEPTH32F image is not guaranteed linear-filterable, MoltenVK included), GBufferNormalTexture through the
        // linear REPEAT state the composite samples the G-buffer with, @p ao as the storage write, @p uniforms
        // (UploadUniforms) as SSAOUB; the pipeline's own setters are the other route. Every access is a compute
        // one, so the pass may run on the async pipe. Not initialised: nothing is declared and Record refuses.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef depth, RDG::TextureRef normal,
                              RDG::TextureRef ao, RDG::BufferRef uniforms ) const
        {
            if ( !m_Pipeline )
                return;
            pass.Bindings( m_BindingLayout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                 .Texture( SSAOBindingShaderParameters::GBufferDepthTexture, depth, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( SSAOBindingShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( SSAOBindingShaderParameters::GBufferNormalTexture, normal, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( SSAOBindingShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                 .Storage( SSAOBindingShaderParameters::SsaoOutputTexture, ao, RDG::Access::StorageWrite )
                 .Uniform( SSAOBindingShaderParameters::SSAOUB, uniforms );
        }

        // EXEC: one thread per AO texel of the @p width x @p height target (SSAO.shader LocalSize(8, 8, 1)).
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context, const uint32_t width,
                                                    const uint32_t height ) const
        {
            if ( !m_Pipeline )
                return Common::MakeError( "Deferred: SSAO: the SSAO pipeline is not initialised" );
            DESERT_PROFILE_PASS( "SSAO" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, GroupCount( width ),
                                                            GroupCount( height ), 1 );
        }

    private:
        static uint32_t GroupCount( const uint32_t texels )
        {
            return ( texels + 7u ) / 8u;
        }

        std::shared_ptr<Shader> m_Shader;
        // The block layout, derived from m_Shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache m_BindingLayout;
        std::shared_ptr<ComputePipeline> m_Pipeline;
    };
} // namespace Desert::Graphic::System

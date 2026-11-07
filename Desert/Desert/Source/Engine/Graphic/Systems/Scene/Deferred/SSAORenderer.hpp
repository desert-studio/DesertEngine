#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/DeferredTileLists.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SSAOUniforms.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <vector>

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
        DESERT_SHADER_PARAMETER( StorageBuffer, DeferredTilesUB );
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
            // GPU-TILE-CLASSIFY: the permutation dispatched over the view's tile list. One that does not build is
            // said here; the view then dispatches over every texel (the same AO).
            m_TiledShader   = nullptr;
            m_TiledPipeline = nullptr;
            auto tiled = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "SSAO", TiledVariant() );
            if ( !tiled )
            {
                LOG_ERROR( "SSAO: the tiled permutation: {} (the view dispatches every texel)", tiled.GetError() );
                return BOOLSUCCESS;
            }
            std::shared_ptr<Shader> tiledShader = tiled.ExtractValue();
            const auto              tiledPipeline =
                 ComputePipeline::Create( { .Shader = tiledShader, .DebugName = "SSAO Tiles" } );
            if ( !tiledPipeline )
            {
                LOG_ERROR( "SSAO: the tiled pipeline: {} (the view dispatches every texel)",
                           tiledPipeline.GetError() );
                return BOOLSUCCESS;
            }
            m_TiledShader   = std::move( tiledShader );
            m_TiledPipeline = tiledPipeline.GetValue();
            return BOOLSUCCESS;
        }

        // SSAO.shader DESERT_DEFERRED_TILES: one workgroup per listed tile of System::DeferredTileLists, cooked.
        [[nodiscard]] static Core::ShaderVariant TiledVariant()
        {
            return Core::ShaderVariant{ .Defines       = { std::string( "DESERT_DEFERRED_TILES" ) },
                                        .ShippedByCook = true };
        }

        // Every permutation the renderer acquires beyond the plain shader, cooked by the package (PackageCook).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants()
        {
            return { { "SSAO", TiledVariant() } };
        }

        // True when a pass over a @p width x @p height AO target may dispatch only the tiles of @p tiles.
        [[nodiscard]] bool DispatchesTiles( const DeferredTileLists& tiles, const uint32_t width,
                                            const uint32_t height ) const noexcept
        {
            return m_TiledPipeline != nullptr && tiles.Covers( width, height );
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
        // @p tiles valid (DispatchesTiles said yes): the tiled permutation, the list read as storage and as the
        // dispatch's indirect record (one use of one buffer, as the deferred lighting's).
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef depth, RDG::TextureRef normal,
                              RDG::TextureRef ao, RDG::BufferRef uniforms, const RDG::BufferRef tiles ) const
        {
            if ( !m_Pipeline )
                return;
            if ( tiles.IsValid() && m_TiledPipeline )
            {
                auto block = pass.Bindings( m_TiledLayout.Get( m_TiledShader ),
                                            Renderer::GetPipelineRouteFill( *m_TiledPipeline ) );
                DeclareBlock( block, depth, normal, ao, uniforms );
                block.Storage( SSAOBindingShaderParameters::DeferredTilesUB, tiles, RDG::Access::StorageRead );
                pass.Read( tiles, RDG::Access::IndirectArgs );
                return;
            }
            auto block =
                 pass.Bindings( m_BindingLayout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) );
            DeclareBlock( block, depth, normal, ao, uniforms );
        }

        // EXEC: one thread per AO texel of the @p width x @p height target (SSAO.shader LocalSize(8, 8, 1)); @p
        // tiles valid: one workgroup per listed tile, the count the classify wrote (the geometry dispatch record).
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context, const uint32_t width,
                                                    const uint32_t height, const RDG::BufferRef tiles ) const
        {
            if ( !m_Pipeline )
                return Common::MakeError( "Deferred: SSAO: the SSAO pipeline is not initialised" );
            DESERT_PROFILE_PASS( "SSAO" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            if ( tiles.IsValid() && m_TiledPipeline )
                return Renderer::DispatchComputeIndirect( bindings, *m_TiledPipeline, tiles,
                                                          DeferredTileLists::kGeometryDispatchOffset );
            return Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, GroupCount( width ),
                                                            GroupCount( height ), 1 );
        }

    private:
        template <typename Block>
        static void DeclareBlock( Block& block, const RDG::TextureRef depth, const RDG::TextureRef normal,
                                  const RDG::TextureRef ao, const RDG::BufferRef uniforms )
        {
            block.Texture( SSAOBindingShaderParameters::GBufferDepthTexture, depth, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( SSAOBindingShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( SSAOBindingShaderParameters::GBufferNormalTexture, normal, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( SSAOBindingShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                 .Storage( SSAOBindingShaderParameters::SsaoOutputTexture, ao, RDG::Access::StorageWrite )
                 .Uniform( SSAOBindingShaderParameters::SSAOUB, uniforms );
        }

        static uint32_t GroupCount( const uint32_t texels )
        {
            return ( texels + 7u ) / 8u;
        }

        std::shared_ptr<Shader> m_Shader;
        // The block layout, derived from m_Shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache m_BindingLayout;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        // The tiled permutation (TiledVariant), its layout and pipeline; null when it did not build.
        std::shared_ptr<Shader>          m_TiledShader;
        mutable ShaderBindingLayoutCache m_TiledLayout;
        std::shared_ptr<ComputePipeline> m_TiledPipeline;
    };
} // namespace Desert::Graphic::System

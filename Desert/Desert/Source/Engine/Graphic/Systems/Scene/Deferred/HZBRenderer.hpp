#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <string_view>

namespace Desert::Graphic::System
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct HZBBuildShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceMipTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMipTexture );
    };

    // Programs/Deferred/HZBSinglePass.shader's slots: the source and the dispatch's up to four mips.
    struct HZBSinglePassShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceMipTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMip0 );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMip1 );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMip2 );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMip3 );
    };

    // Which hierarchy a build makes: the CLOSEST depth (screen traces) or the FURTHEST (occlusion culling, UE
    // FurthestHZB). Reversed-Z: closest = max, furthest = min.
    enum class HZBKind : uint32_t
    {
        Closest  = 0,
        Furthest = 1,
    };

    // THE CLOSEST-DEPTH HIERARCHICAL Z-BUFFER of the view (UE: BuildHZB's ClosestHZB), built from the scene depth
    // once per frame as a transient of the frame graph: mip 0 the depth at full resolution, each mip above it the
    // closest depth of the texels it covers (Programs/Deferred/HZBSinglePass.shader, up to four mips per dispatch
    // through group-shared memory, UE BuildHZB / AMD SPD; RenderPipeline.HZBSinglePass 0 = HZB.shader, one
    // dispatch per mip, the same texels bit for bit - the A/B and the rollback). Screen tracing
    // walks it (Common/HZBTrace.glslh): today SSR's rays; it is the view's, not SSR's - occlusion culling and
    // screen traces of other passes read the same hierarchy.
    class HZBRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "HZB" );
            if ( !m_Shader )
                return Common::MakeError( "HZB shader not found" );
            const auto pipeline = ComputePipeline::Create( { .Shader = m_Shader, .DebugName = "HZB" } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            m_SinglePassShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "HZBSinglePass" );
            if ( !m_SinglePassShader )
                return Common::MakeError( "HZBSinglePass shader not found" );
            const auto singlePass =
                 ComputePipeline::Create( { .Shader = m_SinglePassShader, .DebugName = "HZBSinglePass" } );
            if ( !singlePass )
                return Common::MakeError( singlePass.GetError() );
            m_SinglePassPipeline = singlePass.GetValue();
            return BOOLSUCCESS;
        }

        // RenderPipeline.HZBSinglePass (SceneRenderer::OnUpdate, per frame): true = up to four mips per dispatch
        // (HZBSinglePass.shader), false = one dispatch per mip (HZB.shader). The same hierarchy either way.
        void SetSinglePass( const bool singlePass ) noexcept
        {
            m_SinglePass = singlePass;
        }

        // AddPasses can build a hierarchy (the pipeline exists): a two-phase cull tests last frame's HZB only when
        // this frame's can be built for its phase 2.
        [[nodiscard]] bool CanBuild() const
        {
            return static_cast<bool>( m_Pipeline ) && static_cast<bool>( m_SinglePassPipeline );
        }

        // GRAPH BUILD: the @p kind hierarchy of @p depth (the scene depth, @p width x @p height) as a new transient
        // of @p graph, and the passes that fill it ("HZB: Mip n" / "HZB Furthest: Mip n"). An invalid ref when the
        // pipeline is missing.
        [[nodiscard]] RDG::TextureRef AddPasses( RDG::Builder& graph, const RDG::TextureRef depth,
                                                 const uint32_t width, const uint32_t height,
                                                 const HZBKind kind = HZBKind::Closest )
        {
            if ( !CanBuild() || !depth.IsValid() || width == 0u || height == 0u )
                return {};
            const uint32_t        mips = static_cast<uint32_t>( std::bit_width( std::max( width, height ) ) );
            const RDG::TextureRef hzb =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = ViewTargetFormats::kHZB,
                                                        .Mips   = mips },
                                      kind == HZBKind::Furthest ? "HZB.Furthest" : "HZB" );
            if ( m_SinglePass )
                return AddSinglePassDispatches( graph, depth, hzb, width, height, mips, kind );
            const glm::uvec4 push( static_cast<uint32_t>( kind ), 0u, 0u, 0u );
            for ( uint32_t mip = 0; mip < mips; ++mip )
                graph.AddPass(
                     std::format( "{}: Mip {}", kind == HZBKind::Furthest ? "HZB Furthest" : "HZB", mip ),
                     RDG::PassFlags::Compute,
                     [&, mip]( RDG::PassBuilder& pass )
                     {
                         // Mip 0 reads the depth; every other mip the one below it, as a single-mip view.
                         const RDG::TextureRef       source = mip == 0u ? depth : hzb;
                         const RDG::SubresourceRange range =
                              mip == 0u ? RDG::SubresourceRange::All() : RDG::SubresourceRange::Mip( mip - 1u );
                         pass.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                              .Texture( HZBBuildShaderParameters::SourceMipTexture, source,
                                        RDG::Access::SampledCompute, range )
                              .Sampler( HZBBuildShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Storage( HZBBuildShaderParameters::DestinationMipTexture, hzb,
                                        RDG::Access::StorageWrite, mip )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( push ) ) );
                     },
                     [pipeline = m_Pipeline, mip, width, height,
                      push]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "HZB" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                         return Renderer::GetInstance().DispatchCompute(
                              bindings, *pipeline, GroupCount( std::max( 1u, width >> mip ) ),
                              GroupCount( std::max( 1u, height >> mip ) ), 1 );
                     } );
            return hzb;
        }

    private:
        // HZBSinglePass.shader: mips per dispatch, and the side of the top-mip tile a group owns.
        static constexpr uint32_t kMipsPerDispatch = 4u;
        static constexpr uint32_t kSinglePassTile  = 4u;
        // Up to 16 mips (a 65535-texel side): four dispatches, each named once (no per-frame string).
        static constexpr uint32_t                                     kMaxDispatches = 4u;
        static constexpr std::array<std::string_view, kMaxDispatches> kClosestNames{
             "HZB: Mips 0-3", "HZB: Mips 4-7", "HZB: Mips 8-11", "HZB: Mips 12-15" };
        static constexpr std::array<std::string_view, kMaxDispatches> kFurthestNames{
             "HZB Furthest: Mips 0-3", "HZB Furthest: Mips 4-7", "HZB Furthest: Mips 8-11",
             "HZB Furthest: Mips 12-15" };

        // The single-pass build of @p hzb (@p mips mips): ceil(mips / 4) dispatches, each reading the mip below
        // its first (the depth for mip 0) and writing up to four. A dispatch of fewer than four binds its unused
        // slots to the mips of one scratch transient (distinct subresources, never written by the shader).
        [[nodiscard]] RDG::TextureRef AddSinglePassDispatches( RDG::Builder& graph, const RDG::TextureRef depth,
                                                               const RDG::TextureRef hzb, const uint32_t width,
                                                               const uint32_t height, const uint32_t mips,
                                                               const HZBKind kind )
        {
            const uint32_t dispatches = ( mips + kMipsPerDispatch - 1u ) / kMipsPerDispatch;
            if ( dispatches > kMaxDispatches )
                return {};
            RDG::TextureRef scratch;
            if ( mips % kMipsPerDispatch != 0u )
                scratch = graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = 4u, .Height = 4u },
                                                                 .Format = ViewTargetFormats::kHZB,
                                                                 .Mips   = kMipsPerDispatch - 1u },
                                               "HZB.Scratch" );
            const bool furthest = kind == HZBKind::Furthest;
            for ( uint32_t dispatch = 0; dispatch < dispatches; ++dispatch )
            {
                const uint32_t         first  = dispatch * kMipsPerDispatch;
                const uint32_t         count  = std::min( kMipsPerDispatch, mips - first );
                const uint32_t         topMip = first + count - 1u;
                const glm::uvec4       push( static_cast<uint32_t>( kind ), count, 0u, 0u );
                const uint32_t         groupsX = TileCount( std::max( 1u, width >> topMip ) );
                const uint32_t         groupsY = TileCount( std::max( 1u, height >> topMip ) );
                const std::string_view name    = furthest ? kFurthestNames[dispatch] : kClosestNames[dispatch];
                graph.AddPass(
                     name, RDG::PassFlags::Compute,
                     [&, first, count]( RDG::PassBuilder& pass )
                     {
                         using P = HZBSinglePassShaderParameters;
                         // The first mip reads the depth; every other first mip the one below it, a single mip.
                         const RDG::TextureRef       source = first == 0u ? depth : hzb;
                         const RDG::SubresourceRange range  = first == 0u
                                                                   ? RDG::SubresourceRange::All()
                                                                   : RDG::SubresourceRange::Mip( first - 1u );
                         const auto&                 layout = m_Layout.Get( m_SinglePassShader );
                         auto                        block =
                              pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_SinglePassPipeline ) );
                         block.Texture( P::SourceMipTexture, source, RDG::Access::SampledCompute, range )
                              .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() );
                         const std::array<RDG::StorageTextureParameter, kMipsPerDispatch> slots{
                              P::DestinationMip0, P::DestinationMip1, P::DestinationMip2, P::DestinationMip3 };
                         for ( uint32_t slot = 0; slot < kMipsPerDispatch; ++slot )
                         {
                             if ( slot < count )
                                 block.Storage( slots[slot], hzb, RDG::Access::StorageWrite, first + slot );
                             else
                                 block.Storage( slots[slot], scratch, RDG::Access::StorageWrite, slot - count );
                         }
                         block.PushConstantBytes( static_cast<uint32_t>( sizeof( push ) ) );
                     },
                     [pipeline = m_SinglePassPipeline, groupsX, groupsY,
                      push]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "HZB" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groupsX, groupsY,
                                                                         1 );
                     } );
            }
            return hzb;
        }

        // HZB.shader's LocalSize(8, 8, 1).
        static uint32_t GroupCount( const uint32_t texels )
        {
            return ( texels + 7u ) / 8u;
        }

        // HZBSinglePass.shader: one group per kSinglePassTile x kSinglePassTile texels of the dispatch's top mip.
        static uint32_t TileCount( const uint32_t texels )
        {
            return ( texels + kSinglePassTile - 1u ) / kSinglePassTile;
        }

        std::shared_ptr<Shader>          m_Shader;
        mutable ShaderBindingLayoutCache m_Layout;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        std::shared_ptr<Shader>          m_SinglePassShader;
        std::shared_ptr<ComputePipeline> m_SinglePassPipeline;
        bool                             m_SinglePass = true;
    };
} // namespace Desert::Graphic::System

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
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

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
        DESERT_SHADER_PARAMETER( Texture, SubsurfaceHistoryTexture );
        DESERT_SHADER_PARAMETER( Texture, SubsurfaceHistoryMomentsTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SubsurfaceHistoryOutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SubsurfaceHistoryMomentsOutTexture );
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
        glm::vec4 Params{ 0.0f }; // x scale, y most samples, z frame index, w fewest samples
        glm::mat4 PrevWorldToClip{ 1.0f };
        glm::mat4 PrevClipToWorld{ 1.0f };
        glm::vec4 HistoryParams{ 0.0f }; // x history readable, y PreExposure / PrevPreExposure, z most frames
    };
    static_assert( sizeof( SubsurfaceViewGpu ) == 288 );

    // What the passes read, as refs of THIS frame's graph (CONTRACT §3.5).
    struct SubsurfaceInputs
    {
        RDG::TextureRef SubsurfaceDiffuse; // the composite's split: demodulated diffuse, a = profile row + 1
        RDG::TextureRef GBufferA;          // BaseColor (UE StoredBaseColor)
        RDG::TextureRef GBufferB;          // world normal
        RDG::TextureRef Depth;             // the G-buffer's device depth
    };

    // The view's settings (PostProcessSettings, CONTRACT §6); its matrices come from the ViewFrame.
    struct SubsurfaceSettings
    {
        float Scale = 1.0f;
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
     * The scatter keeps its own temporal history (UE SubsurfaceHistory, r.SSS.Burley.Quality 1): two ping-pong
     * pairs (running mean + frame count, luminance moment + profile row + device depth), read only when they hold the
     * view's previous frame (PassHistory.hpp) at the same Scale and profile rows.
     * Added by SceneRenderer only when the view's SubsurfaceScattering is on and a profile is registered.
     */
    class SubsurfaceRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // AFIS with the variance-adaptive count (UE r.SSS.Burley.Quality 1): a fresh texel draws the most, a
        // converged one as few as its running variance allows; the history averages up to kHistoryFrames
        // estimates.
        static constexpr uint32_t kBurleySamples       = 32;
        static constexpr uint32_t kBurleyFewestSamples = 8;
        static constexpr uint32_t kHistoryFrames       = 32;

        // The previous frame is a different world - see IRenderSystem::OnSceneReplaced.
        void OnSceneReplaced() override
        {
            InvalidateHistory();
        }
        // Camera cut - see IRenderSystem::OnTemporalHistoryReset.
        void OnTemporalHistoryReset() override
        {
            InvalidateHistory();
        }

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
        // Burley" into a new @p width x @p height transient, reading last frame's history and writing this
        // frame's. Empty refs when not ready or nothing is registered.
        [[nodiscard]] SubsurfaceScatter AddScatter( RDG::Builder& graph, const SubsurfaceProfileRegistry& registry,
                                                    const SubsurfaceInputs&   inputs,
                                                    const SubsurfaceSettings& settings, const ViewFrame& frame,
                                                    const uint32_t width, const uint32_t height )
        {
            const std::span<const SubsurfaceProfileRow> rows = registry.Rows();
            if ( !IsReady() || rows.empty() || width == 0u || height == 0u )
                return {};
            // A history scattered at another Scale or with other profiles is another picture.
            const std::span<const std::byte> rowBytes = std::as_bytes( rows );
            if ( settings.Scale != m_HistoryScale || rowBytes.size() != m_HistoryRows.size() ||
                 std::memcmp( rowBytes.data(), m_HistoryRows.data(), rowBytes.size() ) != 0 )
            {
                InvalidateHistory();
                m_HistoryScale = settings.Scale;
                m_HistoryRows.assign( rowBytes.begin(), rowBytes.end() );
            }
            HistorySlot& previous = m_Slots[m_Parity];
            HistorySlot& next     = m_Slots[1u - m_Parity];
            // Decided at build time: the exec runs after the frame was built (PassHistory.hpp).
            const bool readable = previous.Built && previous.Width == width && previous.Height == height &&
                                  previous.Stamp.ReadableIn( frame );
            std::array<RDG::TextureRef, 2> history = { inputs.SubsurfaceDiffuse, inputs.SubsurfaceDiffuse };
            if ( readable )
                history =
                     AcquireSlot( graph, previous, width, height, std::format( "SubsurfaceHistory{}", m_Parity ) );
            const std::array<RDG::TextureRef, 2> written =
                 AcquireSlot( graph, next, width, height, std::format( "SubsurfaceHistory{}", 1u - m_Parity ) );
            m_Parity                   = 1u - m_Parity;
            HistorySlot* const stamped = &next;
            SubsurfaceScatter scatter;
            scatter.Atlas =
                 graph.CreateBuffer( RDG::BufferDesc{ .Bytes = rows.size_bytes() }, "Subsurface: Profile Atlas" );
            graph.QueueBufferUpload( scatter.Atlas, std::as_bytes( rows ) );

            SubsurfaceViewGpu view;
            view.WorldToClip     = frame.JitteredViewProjection;
            view.ClipToWorld     = frame.InvJitteredViewProjection;
            view.Params          = glm::vec4( settings.Scale, static_cast<float>( kBurleySamples ),
                                              static_cast<float>( frame.FrameIndex % 1024u ),
                                              static_cast<float>( kBurleyFewestSamples ) );
            view.PrevWorldToClip = frame.PrevJitteredViewProjection;
            view.PrevClipToWorld = glm::inverse( frame.PrevJitteredViewProjection );
            view.HistoryParams =
                 glm::vec4( readable ? 1.0f : 0.0f,
                            frame.PrevPreExposure > 0.0f ? frame.PreExposure / frame.PrevPreExposure : 1.0f,
                            static_cast<float>( kHistoryFrames ), 0.0f );
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
                                    scatter.Scattered, RDG::Access::StorageWrite )
                          .Texture( SubsurfaceBurleyShaderParameters::SubsurfaceHistoryTexture, history[0], kRead,
                                    all )
                          .Texture( SubsurfaceBurleyShaderParameters::SubsurfaceHistoryMomentsTexture, history[1],
                                    kRead, all )
                          .Storage( SubsurfaceBurleyShaderParameters::SubsurfaceHistoryOutTexture, written[0],
                                    RDG::Access::StorageWrite )
                          .Storage( SubsurfaceBurleyShaderParameters::SubsurfaceHistoryMomentsOutTexture,
                                    written[1], RDG::Access::StorageWrite );
                 },
                 [pipeline = m_BurleyPipeline, stamped, index = frame.FrameIndex, width,
                  height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Subsurface: Burley" );
                     const RDG::PassBindings     bindings( context, context.GetBindingBlock( 0 ) );
                     const Common::BoolResultStr dispatched = Renderer::GetInstance().DispatchCompute(
                          bindings, *pipeline, ( width + 7u ) / 8u, ( height + 7u ) / 8u, 1 );
                     if ( dispatched.IsSuccess() )
                         stamped->Stamp.Stamp( index );
                     return dispatched;
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
        // One history pair: the running mean (rgb) + its frame count (a), and the luminance moment + the texel's
        // identity (profile row, device depth); graph textures created once (CreateTexture +
        // Extract) and registered every frame after (RegisterExternal), as ScreenSpaceDenoiser's slots.
        struct HistorySlot
        {
            RDG::ExternalTexture Mean;
            RDG::ExternalTexture Moments;
            bool                 Built  = false;
            uint32_t             Width  = 0;
            uint32_t             Height = 0;
            PassHistoryStamp     Stamp; // which view frame wrote it
        };

        void InvalidateHistory()
        {
            for ( HistorySlot& slot : m_Slots )
                slot.Stamp.Invalidate();
        }

        static std::array<RDG::TextureRef, 2> AcquireSlot( RDG::Builder& graph, HistorySlot& slot,
                                                           const uint32_t width, const uint32_t height,
                                                           const std::string& name )
        {
            if ( slot.Built && slot.Width == width && slot.Height == height )
                return { graph.RegisterExternal( slot.Mean, name ),
                         graph.RegisterExternal( slot.Moments, name + ".Moments" ) };
            slot.Mean    = {};
            slot.Moments = {};
            slot.Stamp.Invalidate();
            const RDG::TextureRef mean =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = Core::Formats::ImageFormat::RGBA16F },
                                      name );
            const RDG::TextureRef moments =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = Core::Formats::ImageFormat::RGBA32F },
                                      name + ".Moments" );
            graph.Extract( mean, slot.Mean, RDG::Access::SampledCompute );
            graph.Extract( moments, slot.Moments, RDG::Access::SampledCompute );
            slot.Built  = true;
            slot.Width  = width;
            slot.Height = height;
            return { mean, moments };
        }

        std::array<HistorySlot, 2> m_Slots;
        uint32_t                   m_Parity       = 0;
        float                      m_HistoryScale = -1.0f;
        std::vector<std::byte>     m_HistoryRows; // the profile rows the history was scattered with

        std::shared_ptr<Shader>           m_BurleyShader;
        mutable ShaderBindingLayoutCache  m_BurleyLayout;
        std::shared_ptr<ComputePipeline>  m_BurleyPipeline;
        std::shared_ptr<Shader>           m_RecombineShader;
        mutable ShaderBindingLayoutCache  m_RecombineLayout;
        std::shared_ptr<GraphicsPipeline> m_RecombinePipeline;
    };
} // namespace Desert::Graphic::System

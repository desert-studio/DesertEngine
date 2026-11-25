#include <Water/Public/WaterRenderer.hpp>

// THE FFT OCEAN OF ONE VIEW (Tessendorf 2001): the spectrum's upload, the passes that bring it to the view's time,
// its mips and the CPU mirror's read-back. The surface that samples the result is WaterRenderer.cpp's.

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Water/Public/FFTSurfaceMirror.hpp>
#include <Water/Public/WaterWavesService.hpp>
#include <Common/Settings/Scalability.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <utility>
#include <vector>

namespace
{
    // THE FFT OCEAN'S COMPUTE PROGRAMS (Programs/Water/WaterFFT*.shader, WaterWaveMips, WaterWavesStill), one spelling.
    struct WaterFFTShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, WaterFFTInitialSpectrum );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSpectrumA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSpectrumB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSourceA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSourceB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTTargetA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTTargetB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTResultA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTResultB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTWaves );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterWavesSource );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterWavesTarget );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterFFTParamsBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterFFTReadbackBuffer ); // the CPU mirror's texels (finalize)
        static constexpr std::array kSpectrum{ ::Desert::Graphic::RDG::Erase( WaterFFTInitialSpectrum ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTSpectrumA ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTSpectrumB ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTParamsBuffer ) };
        static constexpr std::array kTransform{ ::Desert::Graphic::RDG::Erase( WaterFFTSourceA ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTSourceB ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTTargetA ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTTargetB ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTParamsBuffer ) };
        static constexpr std::array kFinalize{ ::Desert::Graphic::RDG::Erase( WaterFFTResultA ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTResultB ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTWaves ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTParamsBuffer ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTReadbackBuffer ) };
        static constexpr std::array kMips{ ::Desert::Graphic::RDG::Erase( WaterWavesSource ),
                                           ::Desert::Graphic::RDG::Erase( WaterWavesTarget ) };
        static constexpr std::array kStill{ ::Desert::Graphic::RDG::Erase( WaterWavesTarget ) };
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // Water/WaterFFT.glslh WaterFFTParams, std430.
        struct WaterFFTParams
        {
            glm::vec4 Cascade{ 0.0f }; // the patch sides (cm)
            // x = choppiness, y = IFFT direction, z = the view's time (s), w = what the array holds
            glm::vec4 Shape{ 0.0f };
        };
        static_assert( sizeof( WaterFFTParams ) == 2 * sizeof( glm::vec4 ) );
        constexpr uint32_t kFFTN       = Water::kFFTOceanResolution;
        constexpr uint32_t kFFTGroups8 = kFFTN / 8; // LocalSize(8, 8, 1)
        // WATER_WAVES_LAYERS / WATER_WAVES_MIPS.
        constexpr uint32_t kWavesLayers = 4 * Water::kFFTOceanCascades;
        constexpr uint32_t kWavesMips   = Water::kFFTOceanResolutionLog2 + 1;
    } // namespace

    std::shared_ptr<ComputePipeline>
    WaterRenderer::BuildWavePipeline( const char* name, ShaderBindingLayoutCache& layout,
                                      const std::span<const RDG::ShaderParameter> parameters )
    {
        auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
        const auto  shader  = shaders != nullptr ? shaders->GetByName( name ) : nullptr;
        if ( !shader )
        {
            LOG_ERROR( "[Water] the wave simulation's compute shader '{}' is missing", name );
            return nullptr;
        }
        auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
        if ( !pipeline )
        {
            LOG_ERROR( "[Water] '{}' does not build: {}", name, pipeline.GetError() );
            return nullptr;
        }
        if ( const auto valid = layout.Validate( pipeline.GetValue()->GetSpecification().Shader, parameters );
             !valid )
        {
            LOG_ERROR( "[Water] '{}': {}", name, valid.GetError() );
            return nullptr;
        }
        return pipeline.GetValue();
    }

    bool WaterRenderer::EnsureFFTPipelines()
    {
        if ( !m_FFTTried )
        {
            using P        = WaterFFTShaderParameters;
            m_FFTTried     = true;
            m_FFTSpectrum  = BuildWavePipeline( "WaterFFTSpectrum", m_FFTSpectrumLayout, P::kSpectrum );
            m_FFTTransform = BuildWavePipeline( "WaterFFT", m_FFTTransformLayout, P::kTransform );
            m_FFTFinalize  = BuildWavePipeline( "WaterFFTFinalize", m_FFTFinalizeLayout, P::kFinalize );
            m_FFTMips      = BuildWavePipeline( "WaterWaveMips", m_FFTMipsLayout, P::kMips );
        }
        return m_FFTSpectrum != nullptr && m_FFTTransform != nullptr && m_FFTFinalize != nullptr &&
               m_FFTMips != nullptr;
    }

    bool WaterRenderer::PrepareFFTFrame( const Assets::AssetHandle& material, const Assets::AssetHandle& wavesAsset )
    {
        if ( static_cast<uint64_t>( wavesAsset ) == 0 )
            return false;
        const Common::Scalability::ResolvedQuality& quality = Common::Scalability::QualityState::Resolved();
        if ( quality.As<Common::Scalability::WaterWaveMethod>(
                  Common::Scalability::Parameter::EffectsWaterWaveMethod ) !=
             Common::Scalability::WaterWaveMethod::FFT )
            return false;
        auto* const service = Runtime::GetWaterWavesService();
        if ( service == nullptr )
            return false; // PrepareFrame said NoWavesService
        Common::ResultStr<Runtime::WaterWavesService::Spectrum> got = service->GetSpectrum( wavesAsset );
        if ( !got )
        {
            if ( FirstWarning( material, WaterWarning::SpectrumRefused ) )
                Warn( material, std::format( "its body's FFT ocean is refused: {}", got.GetError() ) );
            return false;
        }
        const Runtime::WaterWavesService::Spectrum spectrum = got.GetValue();
        if ( spectrum == nullptr )
            return false; // a Gerstner set, or still loading
        if ( !EnsureFFTPipelines() )
        {
            WarnOnce( material, WaterWarning::FFTPipelinesMissing,
                      "its body's FFT ocean has no compute programs - drawn with its Gerstner generator" );
            return false;
        }

        constexpr size_t kTexels = size_t( kFFTN ) * kFFTN;
        if ( spectrum != m_FFTUploaded )
        {
            std::vector<unsigned char> bytes( Water::kFFTOceanCascades * kTexels * sizeof( glm::vec4 ) );
            for ( uint32_t c = 0; c < Water::kFFTOceanCascades; ++c )
            {
                const std::vector<glm::vec4>& h0 = spectrum->Cascades[c].H0;
                if ( h0.size() != kTexels )
                {
                    if ( FirstWarning( material, WaterWarning::SpectrumMalformed ) )
                        Warn( material, std::format( "its FFT ocean's cascade {} has {} texels, not {} - drawn "
                                                     "with its Gerstner generator",
                                                     c, h0.size(), kTexels ) );
                    return false;
                }
                std::memcpy( bytes.data() + c * kTexels * sizeof( glm::vec4 ), h0.data(),
                             kTexels * sizeof( glm::vec4 ) );
            }
            Core::Formats::Image2DSpecification initial = {
                 .Tag        = "WaterFFTInitialSpectrum",
                 .Width      = kFFTN,
                 .Height     = kFFTN * Water::kFFTOceanCascades,
                 .Format     = Core::Formats::ImageFormat::RGBA32F,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
                 .MipLevels  = {},
            };
            initial.Data = std::move( bytes );
            RetireImage( std::move( m_FFTInitial ) );
            m_FFTInitial  = Image2D::Create( initial );
            m_FFTUploaded = spectrum;
            // The array holds another sea: this frame is its own previous one.
            m_WavesHold = std::make_shared<std::atomic<uint64_t>>( 0 );
        }
        if ( m_FFTInitial == nullptr )
            return false;
        m_FFTFrame = spectrum;
        return true;
    }

    void WaterRenderer::PublishFFTReadback( const uint32_t slot )
    {
        FFTReadbackSlot& read     = m_FFTReadback[slot];
        auto             spectrum = std::exchange( read.Spectrum, nullptr );
        if ( spectrum == nullptr || !read.Buffer.Physical )
            return; // nothing extracted into this slot yet, or its graph faulted before the extract
        const std::span<const std::byte> bytes = read.Buffer.Physical->ReadForHost();
        Water::FFTSurfaceRead            surface;
        surface.Time = read.Time;
        if ( bytes.size() >= Water::kFFTReadbackTexels * sizeof( glm::vec4 ) )
        {
            surface.Texels.resize( Water::kFFTReadbackTexels );
            std::memcpy( surface.Texels.data(), bytes.data(), surface.Texels.size() * sizeof( glm::vec4 ) );
        }
        else
            WarnOnce( Assets::AssetHandle::Null(), WaterWarning::FFTReadbackUnreadable,
                      std::format( "[Water] the FFT read-back is not host-readable ({} bytes): what floats rides "
                                   "the still plane",
                                   bytes.size() ) );
        read.Buffer.Physical.reset();
        if ( !surface.Texels.empty() )
            Runtime::GetWaterWavesService()->Surfaces().Publish( spectrum, std::move( surface ) );
    }

    RDG::TextureRef WaterRenderer::AddStillWaves( RDG::Builder& graph )
    {
        if ( !m_WavesStillTried )
        {
            m_WavesStillTried = true;
            m_WavesStill =
                 BuildWavePipeline( "WaterWavesStill", m_WavesStillLayout, WaterFFTShaderParameters::kStill );
        }
        if ( m_WavesStill == nullptr )
            return {};
        // No FFT ocean draws: one texel of rest per layer, what the surface's array slot is bound to.
        const RDG::TextureRef still = graph.CreateTexture( RDG::TextureDesc{ .Size   = { 1, 1, 1 },
                                                                             .Format = Core::Formats::ImageFormat::RGBA16F,
                                                                             .Layers = kWavesLayers,
                                                                             .Dim    = RDG::TextureDim::Tex2DArray },
                                                           "Water.WavesStill" );
        graph.AddPass(
             "Water: Still Waves", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_WavesStillLayout.Get( m_WavesStill->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_WavesStill ) )
                      .Storage( WaterFFTShaderParameters::WaterWavesTarget, still, RDG::Access::StorageWrite, 0 );
             },
             [pipeline = m_WavesStill]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, 1u, 1u, kWavesLayers );
             } );
        return still;
    }

    WaterFrameRefs WaterRenderer::AddFramePasses( RDG::Builder& graph, const WaterFrameImports& imports,
                                                  const float timeSeconds )
    {
        if ( const WaterFrameRefs recorded = RecordedFrame( graph ); recorded.IsValid() )
            return recorded;
        if ( m_Walk.Programs.empty() )
            return {};
        WaterFrameRefs refs;
        refs.Shore = AddShorePass( graph );
        refs.River = imports.River;
        refs.Waves = FrameWavesAreFFT() && imports.Spectrum.IsValid()
                          ? AddWavePasses( graph, imports.Spectrum, timeSeconds )
                          : AddStillWaves( graph );
        if ( !refs.Waves.IsValid() || !refs.Shore.IsValid() || !refs.River.IsValid() )
            return {};
        refs.Foam          = AddFoamPass( graph, refs, imports, timeSeconds );
        m_FrameWaterRefs   = refs;
        m_FrameWaterGraph  = &graph;
        return refs;
    }

    RDG::TextureRef WaterRenderer::AddWavePasses( RDG::Builder& graph, const RDG::TextureRef initial,
                                                  const float timeSeconds )
    {
        const auto& sizes = m_FFTFrame->Params.CascadeSizes;
        // What the array holds, by the frame whose graph completed it last: this frame (a second view's graph
        // rewrites the same waves and keeps the same previous frame), the one before it, or nothing this frame
        // may call its previous one (a gap, a faulted graph, another sea).
        const uint64_t frame     = ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint64_t completed = m_WavesHold->load( std::memory_order_acquire ); // frame + 1; 0: none
        float          holds     = 0.0f;
        if ( completed == frame + 1 )
            holds = 2.0f;
        else if ( completed != 0 && completed == frame )
            holds = 1.0f;
        const RDG::TextureRef waves =
             m_WavesImage.Physical
                  ? graph.RegisterExternal( m_WavesImage, "Water.Waves" )
                  : graph.CreateTexture( RDG::TextureDesc{ .Size   = { kFFTN, kFFTN, 1 },
                                                           .Format = Core::Formats::ImageFormat::RGBA16F,
                                                           .Mips   = kWavesMips,
                                                           .Layers = kWavesLayers,
                                                           .Dim    = RDG::TextureDim::Tex2DArray },
                                         "Water.Waves" );
        WaterFFTParams rows{ .Cascade = glm::vec4( sizes[0], sizes[1], sizes[2], sizes[3] ),
                             .Shape   = glm::vec4( m_FFTFrame->Params.Choppiness, 0.0f, timeSeconds, holds ) };
        WaterFFTParams       columns = rows;
        columns.Shape.y              = 1.0f;
        const RDG::BufferRef rowsBuffer = graph.CreateBuffer( RDG::BufferDesc{ sizeof( WaterFFTParams ) },
                                                              "WaterFFT.ParamsRows" );
        const RDG::BufferRef columnsBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( WaterFFTParams ) }, "WaterFFT.ParamsColumns" );
        graph.QueueBufferUpload( rowsBuffer, std::as_bytes( std::span( &rows, 1 ) ) );
        graph.QueueBufferUpload( columnsBuffer, std::as_bytes( std::span( &columns, 1 ) ) );

        const RDG::TextureDesc spectrumDesc{ .Size   = { kFFTN, kFFTN, 1 },
                                             .Format = Core::Formats::ImageFormat::RGBA32F,
                                             .Layers = Water::kFFTOceanCascades,
                                             .Dim    = RDG::TextureDim::Tex2D };
        const RDG::TextureRef specA = graph.CreateTexture( spectrumDesc, "WaterFFT.SpectrumA" );
        const RDG::TextureRef specB = graph.CreateTexture( spectrumDesc, "WaterFFT.SpectrumB" );
        const RDG::TextureRef rowA  = graph.CreateTexture( spectrumDesc, "WaterFFT.RowsA" );
        const RDG::TextureRef rowB  = graph.CreateTexture( spectrumDesc, "WaterFFT.RowsB" );
        const RDG::TextureRef colA  = graph.CreateTexture( spectrumDesc, "WaterFFT.ColumnsA" );
        const RDG::TextureRef colB  = graph.CreateTexture( spectrumDesc, "WaterFFT.ColumnsB" );

        using P = WaterFFTShaderParameters;
        graph.AddPass(
             "Water: FFT Spectrum", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_FFTSpectrumLayout.Get( m_FFTSpectrum->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_FFTSpectrum ) )
                      .Texture( P::WaterFFTInitialSpectrum, initial, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Storage( P::WaterFFTSpectrumA, specA, RDG::Access::StorageWrite, 0 )
                      .Storage( P::WaterFFTSpectrumB, specB, RDG::Access::StorageWrite, 0 )
                      .Storage( P::WaterFFTParamsBuffer, rowsBuffer, RDG::Access::StorageRead );
             },
             [pipeline = m_FFTSpectrum]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, kFFTGroups8, kFFTGroups8,
                                                                 Water::kFFTOceanCascades );
             } );
        // Rows, then columns: one 256-thread group per line per cascade (WaterFFT.shader LocalSize(256, 1, 1)).
        const auto transform = [&]( const char* name, const RDG::TextureRef srcA, const RDG::TextureRef srcB,
                                    const RDG::TextureRef dstA, const RDG::TextureRef dstB,
                                    const RDG::BufferRef params )
        {
            graph.AddPass(
                 name, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_FFTTransformLayout.Get( m_FFTTransform->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_FFTTransform ) )
                          .Storage( P::WaterFFTSourceA, srcA, RDG::Access::StorageRead, 0 )
                          .Storage( P::WaterFFTSourceB, srcB, RDG::Access::StorageRead, 0 )
                          .Storage( P::WaterFFTTargetA, dstA, RDG::Access::StorageWrite, 0 )
                          .Storage( P::WaterFFTTargetB, dstB, RDG::Access::StorageWrite, 0 )
                          .Storage( P::WaterFFTParamsBuffer, params, RDG::Access::StorageRead );
                 },
                 [pipeline = m_FFTTransform]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, 1u, kFFTN,
                                                                     Water::kFFTOceanCascades );
                 } );
        };
        transform( "Water: FFT Rows", specA, specB, rowA, rowB, rowsBuffer );
        transform( "Water: FFT Columns", rowA, rowB, colA, colB, columnsBuffer );
        // THE CPU MIRROR's slot: this frame in flight's buffer, read (and published) before it is written again.
        const uint32_t slot  = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint32_t slots = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( m_FFTReadback.size() != slots )
            m_FFTReadback.assign( slots, FFTReadbackSlot{} );
        // A second view's graph of one frame leaves the slot to the first: its read-back is of this very frame.
        const bool readback = slot < slots && !( m_FFTReadback[slot].Spectrum != nullptr &&
                                                 m_FFTReadback[slot].Frame == frame );
        if ( readback )
            PublishFFTReadback( slot );
        const RDG::BufferRef readbackBuffer = graph.CreateBuffer(
             RDG::BufferDesc{ Water::kFFTReadbackTexels * sizeof( glm::vec4 ) }, "WaterFFT.Readback" );
        // StorageWrite is read-modify-write: the displacement the array holds becomes the previous frame's layers.
        graph.AddPass(
             "Water: FFT Finalize", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_FFTFinalizeLayout.Get( m_FFTFinalize->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_FFTFinalize ) )
                      .Storage( P::WaterFFTResultA, colA, RDG::Access::StorageRead, 0 )
                      .Storage( P::WaterFFTResultB, colB, RDG::Access::StorageRead, 0 )
                      .Storage( P::WaterFFTWaves, waves, RDG::Access::StorageWrite, 0 )
                      .Storage( P::WaterFFTParamsBuffer, rowsBuffer, RDG::Access::StorageRead )
                      .Storage( P::WaterFFTReadbackBuffer, readbackBuffer, RDG::Access::StorageWrite );
             },
             [pipeline = m_FFTFinalize, hold = m_WavesHold, frame]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 Common::BoolResultStr   done = Renderer::GetInstance().DispatchCompute(
                      bindings, *pipeline, kFFTGroups8, kFFTGroups8, Water::kFFTOceanCascades );
                 if ( done )
                     hold->store( frame + 1, std::memory_order_release );
                 return done;
             } );
        // The mips: a distant pixel reads the mean of the waves under it, and their moments say what it lost.
        for ( uint32_t mip = 0; mip + 1 < kWavesMips; ++mip )
        {
            const uint32_t side = std::max( kFFTN >> ( mip + 1 ), 1u );
            graph.AddPass(
                 "Water: Wave Mips", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_FFTMipsLayout.Get( m_FFTMips->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_FFTMips ) )
                          .Storage( P::WaterWavesSource, waves, RDG::Access::StorageRead, mip )
                          .Storage( P::WaterWavesTarget, waves, RDG::Access::StorageWrite, mip + 1 );
                 },
                 [pipeline = m_FFTMips, groups = ( side + 7u ) / 8u]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups, groups, kWavesLayers );
                 } );
        }
        if ( readback )
        {
            graph.Extract( readbackBuffer, m_FFTReadback[slot].Buffer, RDG::Access::HostRead );
            m_FFTReadback[slot].Spectrum = m_FFTFrame;
            m_FFTReadback[slot].Time     = static_cast<double>( timeSeconds );
            m_FFTReadback[slot].Frame    = frame;
        }
        graph.Extract( waves, m_WavesImage, RDG::Access::SampledGraphics,
                       { RDG::Access::StorageRead, RDG::Access::StorageWrite, RDG::Access::SampledCompute } );
        return waves;
    }
} // namespace Desert::Graphic::System

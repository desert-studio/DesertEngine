#include <Water/Public/WaterRenderer.hpp>

// THE FOAM OF ONE VIEW (Water/WaterFoamField.glslh, Programs/Water/WaterFoamUpdate.shader): a clipmap that remembers
// where the waves broke, where the beach is wet and what the currents carried.

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Common/Settings/Scalability.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

namespace
{
    struct WaterFoamShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterTiles );
        DESERT_SHADER_PARAMETER( Texture, WaterShoreTexture );
        DESERT_SHADER_PARAMETER( Texture, WaterRiverTexture );
        DESERT_SHADER_PARAMETER( Texture, WaterWavesTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( Texture, FoamHistoryTexture );
        DESERT_SHADER_PARAMETER( UniformBuffer, WaterFoamUB );
        static constexpr std::array kAll{
             ::Desert::Graphic::RDG::Erase( WaterTiles ),         ::Desert::Graphic::RDG::Erase( WaterShoreTexture ),
             ::Desert::Graphic::RDG::Erase( WaterRiverTexture ),  ::Desert::Graphic::RDG::Erase( WaterWavesTexture ),
             ::Desert::Graphic::RDG::Erase( LinearRepeatSampler ), ::Desert::Graphic::RDG::Erase( FoamHistoryTexture ),
             ::Desert::Graphic::RDG::Erase( WaterFoamUB ) };
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // Programs/Water/WaterFoamUpdate.shader WaterFoamUB, std140.
        struct WaterFoamUBData
        {
            std::array<glm::vec4, Water::kFoamLevels> Previous{}; // xy = the history's window origins
            glm::vec4 Time{ 0.0f };   // x = the time (s), y = since the history (s), z = 1 with a history, w = still Y
            glm::vec4 Source{ 0.0f }; // x = jacobian threshold, y = softness, z = gain, w = the wave set (-1: none)
            glm::vec4 Decay{ 0.0f };  // x = foam, y = bubbles, z = wetness (s), w = the beach's slope
            glm::vec4 Wind{ 0.0f };   // xy = cm/s
        };
        static_assert( sizeof( WaterFoamUBData ) == 7 * sizeof( glm::vec4 ) );
        constexpr Core::Formats::ImageFormat kFoamFormat = Core::Formats::ImageFormat::RGBA8F;
        // A history older than this is another sea.
        constexpr float kFoamHistoryMaxAgeSeconds = 2.0f;
        // The wind drags the surface at 3 % of its speed (the classic Ekman surface drift).
        constexpr float kSurfaceDriftOfWind = 0.03f;
    } // namespace

    bool WaterRenderer::EnsureFoam( const uint32_t side )
    {
        const uint64_t frame = ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( m_FoamSide != side )
        {
            for ( std::shared_ptr<Image2D>& image : m_Foam )
            {
                const Core::Formats::Image2DSpecification spec = { .Tag    = "WaterFoam",
                                                                   .Width  = side * Water::kFoamLevels,
                                                                   .Height = side,
                                                                   .Format = kFoamFormat,
                                                                   .Usage  = Core::Formats::Image2DUsage::Attachment,
                                                                   .Properties =
                                                                        Core::Formats::Sample |
                                                                        Core::Formats::GraphDeclaredBindless };
                if ( image != nullptr )
                    m_RetiredFoam.Park( std::move( image ), frame );
                image = Image2D::Create( spec );
                if ( image == nullptr || !image->GetBindlessSlot().IsValid() )
                {
                    LOG_ERROR( "[Water] the {}x{} foam atlas was not created with a bindless slot: no foam",
                               side * Water::kFoamLevels, side );
                    image = nullptr;
                }
            }
            m_FoamSide        = side; // a refused size is said once, not retried every frame
            m_FoamWritten     = 0;
            m_FoamState.Valid = false;
            m_FoamRecorded.reset();
        }
        if ( !m_FoamTried )
        {
            m_FoamTried = true;
            if ( const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "WaterFoamUpdate" ) )
            {
                GraphicsPipelineSpecification spec;
                spec.DebugName         = "WaterFoamUpdate";
                spec.Shader            = shader;
                spec.TargetLayout      = RenderTargetLayout{ { kFoamFormat }, {}, 1 };
                spec.DepthTestEnabled  = false;
                spec.DepthWriteEnabled = false;
                spec.CullMode          = CullMode::None;
                spec.Topology          = PrimitiveTopology::Triangles;
                spec.BlendEnable       = false;
                const auto pipeline    = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
                const auto valid       = m_FoamLayout.Validate( shader, WaterFoamShaderParameters::kAll );
                if ( !pipeline )
                    LOG_ERROR( "[Water] WaterFoamUpdate does not build: {}", pipeline.GetError() );
                else if ( !valid )
                    LOG_ERROR( "[Water] WaterFoamUpdate: {}", valid.GetError() );
                else
                    m_FoamPipeline = pipeline.GetValue();
            }
            else
                LOG_ERROR( "[Water] the foam's shader 'WaterFoamUpdate' is missing: no foam" );
        }
        return m_Foam[0] != nullptr && m_Foam[1] != nullptr && m_FoamPipeline != nullptr;
    }

    void WaterRenderer::PrepareFoam( const Assets::AssetHandle& material, const MaterialStamp& stamp,
                                     const glm::vec3& eye, const int32_t waveSet,
                                     const std::shared_ptr<const Water::WaterShoreField>& shore,
                                     std::array<glm::vec4, 3>& origin, glm::vec4& shape )
    {
        const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
        m_RetiredFoam.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
        const auto side = Common::Scalability::QualityState::Resolved().As<uint32_t>(
             Common::Scalability::Parameter::EffectsWaterFoamSize );
        if ( !EnsureFoam( side ) || m_FrameBodies.empty() )
            return;
        // The frame that recorded the pass is over: what it wrote is the history now.
        if ( m_FoamRecorded )
        {
            m_FoamWritten ^= 1u;
            m_FoamState.Origin = *m_FoamRecorded;
            m_FoamState.Valid  = true;
            m_FoamTime         = m_FoamRecordedTime;
            m_FoamRecorded.reset();
        }
        if ( m_FoamShore != shore )
        {
            m_FoamShore       = shore; // another floor: the foam and the wet sand were another shore's
            m_FoamState.Valid = false;
        }

        if ( m_FoamMaterial.Material != static_cast<uint64_t>( material ) || !( m_FoamMaterial.Stamp == stamp ) )
        {
            m_FoamMaterial.Material = static_cast<uint64_t>( material );
            m_FoamMaterial.Stamp    = stamp;
            constexpr std::array<std::string_view, 6> kNames{ "FoamDecaySeconds", "BubbleDecaySeconds",
                                                              "WetDrySeconds",    "WhitecapGain",
                                                              "FoamJacobianThreshold",
                                                              "FoamJacobianSoftness" };
            std::array<glm::vec4, 6>                  values{};
            if ( ReadMaterialValues( material, stamp, kNames, values ) )
            {
                m_FoamMaterial.Params.FoamDecaySeconds   = std::max( values[0].x, 0.1f );
                m_FoamMaterial.Params.BubbleDecaySeconds = std::max( values[1].x, 0.1f );
                m_FoamMaterial.Params.WetDrySeconds      = std::max( values[2].x, 0.1f );
                m_FoamMaterial.Params.WhitecapGain       = std::max( values[3].x, 0.0f );
                m_FoamMaterial.Params.JacobianThreshold  = values[4].x;
                m_FoamMaterial.Params.JacobianSoftness   = std::max( values[5].x, 0.01f );
            }
        }

        const Water::WaterFoamClipmapDesc desc{ .Size = side };
        const auto                        windows = Water::PlaceFoamWindows( desc, glm::vec2( eye.x, eye.z ) );
        for ( uint32_t l = 0; l < Water::kFoamLevels; ++l )
            origin[l] = glm::vec4( glm::vec2( windows[l] ), 1.0f / desc.TexelCm( l ), 0.0f );
        shape = glm::vec4( static_cast<float>( side ), 1.0f, 0.0f, 0.0f );

        FoamFrame frame;
        frame.Params  = m_FoamMaterial.Params;
        frame.Origin  = windows;
        frame.WaveSet = waveSet;
        frame.StillY  = shore != nullptr ? shore->ReferenceLevelCm : m_FrameBodies.front().SurfaceY;
        if ( m_FFTFrame != nullptr )
        {
            const float angle = glm::radians( m_FFTFrame->Wind.AngleDeg );
            frame.Wind        = glm::vec2( std::cos( angle ), std::sin( angle ) ) * m_FFTFrame->Wind.Speed;
        }
        frame.BeachSlope = shore != nullptr ? shore->BeachSlope : 0.0f;
        m_FoamFrame      = frame;
    }

    ShoreFoamUniforms WaterRenderer::GetShoreFoamUniforms() const
    {
        // The ground reads what the last frame wrote, where it wrote it.
        ShoreFoamUniforms uniforms;
        const auto&       image = m_Foam[m_FoamWritten];
        if ( image == nullptr || !m_FoamState.Valid )
            return uniforms;
        const Water::WaterFoamClipmapDesc desc{ .Size = m_FoamSide };
        for ( uint32_t l = 0; l < Water::kFoamLevels; ++l )
            uniforms.Origin[l] = glm::vec4( glm::vec2( m_FoamState.Origin[l] ), 1.0f / desc.TexelCm( l ), 0.0f );
        uniforms.Shape = glm::vec4( static_cast<float>( m_FoamSide ), 1.0f,
                                    static_cast<float>( image->GetBindlessSlot().Index ), 0.0f );
        return uniforms;
    }

    RDG::TextureRef WaterRenderer::AddFoamPass( RDG::Builder& graph, const WaterFrameRefs& water,
                                                const WaterFrameImports& imports, const float timeSeconds )
    {
        const std::span<const std::byte> waves = GetFrameWavesBlock();
        if ( !m_FoamFrame || m_FoamPipeline == nullptr || waves.empty() || !imports.FoamTarget.IsValid() ||
             !imports.FoamHistory.IsValid() )
            return imports.Black;
        const FoamFrame& frame = *m_FoamFrame;
        const float      age   = timeSeconds - m_FoamTime;
        const bool       held  = m_FoamState.Valid && age >= 0.0f && age <= kFoamHistoryMaxAgeSeconds;

        WaterFoamUBData data;
        for ( uint32_t l = 0; l < Water::kFoamLevels; ++l )
            data.Previous[l] = glm::vec4( glm::vec2( m_FoamState.Origin[l] ), 0.0f, 0.0f );
        data.Time   = glm::vec4( timeSeconds, held ? age : 0.0f, held ? 1.0f : 0.0f, frame.StillY );
        data.Source = glm::vec4( frame.Params.JacobianThreshold, frame.Params.JacobianSoftness,
                                 frame.Params.WhitecapGain, static_cast<float>( frame.WaveSet ) );
        data.Decay  = glm::vec4( frame.Params.FoamDecaySeconds, frame.Params.BubbleDecaySeconds,
                                 frame.Params.WetDrySeconds, frame.BeachSlope );
        data.Wind   = glm::vec4( frame.Wind * kSurfaceDriftOfWind, 0.0f, 0.0f );

        const RDG::BufferRef uniforms = graph.CreateBuffer( RDG::BufferDesc{ sizeof( data ) }, "Water.FoamUB" );
        graph.QueueBufferUpload( uniforms, std::as_bytes( std::span<const WaterFoamUBData>( &data, 1 ) ) );
        const RDG::BufferRef tiles = graph.CreateBuffer( RDG::BufferDesc{ waves.size_bytes() }, "Water.FoamWaves" );
        graph.QueueBufferUpload( tiles, waves );

        using P = WaterFoamShaderParameters;
        graph.AddPass(
             "Water: Foam", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.ColorTarget( 0, imports.FoamTarget, RDG::LoadOp::DontCare() );
                 pass.Bindings( m_FoamLayout.Get( m_FoamPipeline->GetSpecification().Shader ), RDG::NoOtherRouteFill() )
                      .Storage( P::WaterTiles, tiles, RDG::Access::StorageRead )
                      .Texture( P::WaterShoreTexture, water.Shore, RDG::Access::SampledGraphics,
                                RDG::SubresourceRange::All() )
                      .Texture( P::WaterRiverTexture, water.River, RDG::Access::SampledGraphics,
                                RDG::SubresourceRange::All() )
                      .Texture( P::WaterWavesTexture, water.Waves, RDG::Access::SampledGraphics,
                                RDG::SubresourceRange::All() )
                      .Sampler( P::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                      .Texture( P::FoamHistoryTexture, imports.FoamHistory, RDG::Access::SampledGraphics,
                                RDG::SubresourceRange::All() )
                      .Uniform( P::WaterFoamUB, uniforms );
             },
             [pipeline = m_FoamPipeline]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DrawFullscreen( bindings, *pipeline, nullptr );
             } );
        m_FoamRecorded     = frame.Origin;
        m_FoamRecordedTime = timeSeconds;
        return imports.FoamTarget;
    }
} // namespace Desert::Graphic::System

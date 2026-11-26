#include <Water/Public/WaterRenderer.hpp>

// THE FOAM OF ONE VIEW (Water/WaterFoamField.glslh, Programs/Water/WaterFoamUpdate.shader): a clipmap that remembers
// where the waves broke, where the beach is wet and what the currents carried.

#include <Engine/Graphic/PipelineCache.hpp>
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
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterFoamBodies );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterFoamPlunges );
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
             ::Desert::Graphic::RDG::Erase( WaterFoamUB ),         ::Desert::Graphic::RDG::Erase( WaterFoamBodies ),
             ::Desert::Graphic::RDG::Erase( WaterFoamPlunges ) };
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
            glm::vec4 Time{ 0.0f }; // x = the time (s), y = since the history (s), z = 1 with a history, w = the bodies
            // The rectangle of the world the history does not stand in (min X, min Z, max X, max Z); empty: min > max.
            glm::vec4 Changed{ 1.0f, 1.0f, 0.0f, 0.0f };
            glm::vec4 PlungeCount{ 0.0f }; // x = the plunges in WaterFoamPlunges
        };
        static_assert( sizeof( WaterFoamUBData ) == 6 * sizeof( glm::vec4 ) );
        constexpr Core::Formats::ImageFormat kFoamFormat = Core::Formats::ImageFormat::RGBA8F;
        // A history older than this is another sea.
        constexpr float kFoamHistoryMaxAgeSeconds = 2.0f;
        // The wind drags the surface at 3 % of its speed (the classic Ekman surface drift).
        constexpr float kSurfaceDriftOfWind = 0.03f;
    } // namespace

    void WaterRenderer::RetireImage( std::shared_ptr<Image2D> image )
    {
        if ( image != nullptr )
            m_RetiredImages.Park( std::move( image ),
                                  ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
    }

    bool WaterRenderer::EnsureFoam( const uint32_t side )
    {
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
                RetireImage( std::move( image ) );
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
                {
                    LOG_ERROR( "[Water] WaterFoamUpdate does not build: {}", pipeline.GetError() );
                }
                else if ( !valid )
                {
                    LOG_ERROR( "[Water] WaterFoamUpdate: {}", valid.GetError() );
                }
                else
                {
                    m_FoamPipeline = pipeline.GetValue();
                }
            }
            else
                LOG_ERROR( "[Water] the foam's shader 'WaterFoamUpdate' is missing: no foam" );
        }
        return m_Foam[0] != nullptr && m_Foam[1] != nullptr && m_FoamPipeline != nullptr;
    }

    const WaterRenderer::BodyMaterialValues& WaterRenderer::MaterialValuesOf( const Assets::AssetHandle& material,
                                                                             const MaterialStamp&       stamp )
    {
        if ( !( m_BodyMaterialStamp == stamp ) )
        {
            m_BodyMaterialStamp = stamp;
            m_BodyMaterials.clear();
        }
        const auto [known, fresh] = m_BodyMaterials.try_emplace( static_cast<uint64_t>( material ) );
        BodyMaterialValues& values = known->second;
        if ( !fresh )
            return values;
        constexpr std::array<std::string_view, 6> kFoam{ "FoamDecaySeconds",      "BubbleDecaySeconds",
                                                         "WetDrySeconds",         "WhitecapGain",
                                                         "FoamJacobianThreshold", "FoamJacobianSoftness" };
        std::array<glm::vec4, 6>                  foam{};
        if ( ReadMaterialValues( material, stamp, kFoam, foam ) )
        {
            values.Foam.FoamDecaySeconds   = std::max( foam[0].x, 0.1f );
            values.Foam.BubbleDecaySeconds = std::max( foam[1].x, 0.1f );
            values.Foam.WetDrySeconds      = std::max( foam[2].x, 0.1f );
            values.Foam.WhitecapGain       = std::max( foam[3].x, 0.0f );
            values.Foam.JacobianThreshold  = foam[4].x;
            values.Foam.JacobianSoftness   = std::max( foam[5].x, 0.01f );
        }
        else if ( ProgramFor( material, stamp ) != nullptr ) // no program is named by ProgramFor itself
            WarnOnce( material, WaterWarning::FoamMissing,
                      "its program declares no FoamDecaySeconds / BubbleDecaySeconds / WetDrySeconds / WhitecapGain "
                      "/ FoamJacobianThreshold / FoamJacobianSoftness: its foam runs on the simulation's values" );
        constexpr std::array<std::string_view, 3> kCaustics{ "CausticsOnsetDepth", "CausticsFadeDepth",
                                                             "CausticsStrength" };
        std::array<glm::vec4, 3>                  caustics{};
        if ( ReadMaterialValues( material, stamp, kCaustics, caustics ) )
            values.Caustics = glm::vec3( std::max( caustics[0].x, 1.0f ), std::max( caustics[1].x, 1.0f ),
                                         std::clamp( caustics[2].x, 0.0f, 1.0f ) );
        else if ( ProgramFor( material, stamp ) != nullptr )
            WarnOnce( material, WaterWarning::CausticsMissing,
                      "its program declares no CausticsOnsetDepth / CausticsFadeDepth / CausticsStrength: it casts "
                      "no caustics" );
        return values;
    }

    void WaterRenderer::PrepareFoam( const MaterialStamp& stamp, const glm::vec3& eye, const int32_t fftSet,
                                     const std::shared_ptr<const Water::WaterShoreField>&           shore,
                                     std::span<const std::shared_ptr<const Water::RiverFlowField>> rivers,
                                     std::array<glm::vec4, 3>& origin, glm::vec4& shape )
    {
        const auto side = Common::Scalability::QualityState::Resolved().As<uint32_t>(
             Common::Scalability::Parameter::EffectsWaterFoamSize );
        if ( !EnsureFoam( side ) ||
             std::ranges::none_of( m_FrameBodyLooks, []( const FrameBodyLook& look ) { return look.River >= -1; } ) )
            return;
        // The frame that recorded the pass is over: what it wrote is the history now.
        if ( m_FoamRecorded )
        {
            m_FoamWritten ^= 1u;
            m_FoamState.Origin = *m_FoamRecorded;
            m_FoamState.Valid  = true;
            m_FoamTime         = m_FoamRecordedTime;
            m_FoamRecorded.reset();
            m_FoamChanged.reset();
        }
        if ( const uint64_t floor = shore != nullptr ? shore->Revision : 0; m_FoamShore != floor )
        {
            // Another floor: the foam and the wet sand go where the ground moved, and everywhere on another grid.
            if ( shore != nullptr && shore->BaseRevision != 0 && shore->BaseRevision == m_FoamShore )
            {
                if ( const Water::WaterShoreField::Rect& rect = shore->ChangedGround; !rect.Empty() )
                {
                    const glm::vec2 lo = shore->MinCm + ( glm::vec2( rect.X0, rect.Z0 ) - 1.0f ) * shore->TexelCm;
                    const glm::vec2 hi = shore->MinCm + ( glm::vec2( rect.X1, rect.Z1 ) + 1.0f ) * shore->TexelCm;
                    const glm::vec4 held = m_FoamChanged.value_or( glm::vec4( lo, hi ) );
                    m_FoamChanged        = glm::vec4( glm::min( glm::vec2( held.x, held.y ), lo ),
                                                      glm::max( glm::vec2( held.z, held.w ), hi ) );
                }
            }
            else
                m_FoamState.Valid = false;
            m_FoamShore = floor;
        }

        const Water::WaterFoamClipmapDesc desc{ .Size = side };
        const auto                        windows = Water::PlaceFoamWindows( desc, glm::vec2( eye.x, eye.z ) );
        for ( uint32_t l = 0; l < Water::kFoamLevels; ++l )
            origin[l] = glm::vec4( glm::vec2( windows[l] ), 1.0f / desc.TexelCm( l ), 0.0f );
        shape = glm::vec4( static_cast<float>( side ), 1.0f, 0.0f, 0.0f );

        FoamFrame frame;
        frame.Origin = windows;
        glm::vec2 drift( 0.0f );
        if ( m_FFTFrame != nullptr )
        {
            const float angle = glm::radians( m_FFTFrame->Wind.AngleDeg );
            drift = glm::vec2( std::cos( angle ), std::sin( angle ) ) * ( m_FFTFrame->Wind.Speed * kSurfaceDriftOfWind );
        }
        const float slope = shore != nullptr ? shore->BeachSlope : 0.0f;
        frame.Bodies.reserve( m_FrameBodies.size() );
        for ( size_t b = 0; b < m_FrameBodies.size(); ++b )
        {
            const Water::WaterQuadTreeBody& body   = m_FrameBodies[b];
            const FrameBodyLook&            look   = m_FrameBodyLooks[b];
            if ( look.River < -1 )
                continue; // a river with no bake has no banks: no water of it stands anywhere
            const Water::WaterFoamParams    params = MaterialValuesOf( look.Material, stamp ).Foam;
            const int32_t                   set    = look.River >= 0 ? -1 : static_cast<int32_t>( m_FrameBodyWaveLanes[b] ) - 1;
            frame.Bodies.push_back( FoamBody{
                 .Footprint = glm::vec4( body.FootprintMin, body.FootprintMax ),
                 .Level     = glm::vec4( body.SurfaceY, static_cast<float>( set ), static_cast<float>( look.River + 1 ),
                                         look.River >= 0 ? 0.0f : slope ),
                 .Source    = glm::vec4( params.JacobianThreshold, params.JacobianSoftness, params.WhitecapGain, 0.0f ),
                 .Decay = glm::vec4( params.FoamDecaySeconds, params.BubbleDecaySeconds, params.WetDrySeconds, 0.0f ),
                 .Drift = set >= 0 && set == fftSet ? glm::vec4( drift, 0.0f, 0.0f ) : glm::vec4( 0.0f ) } );
        }
        // THE BOILS: every fall's plunge (Water::FallPlunge, baked once with the river) whose circle reaches the
        // outer window; the update makes its foam and spreads it (WaterPlungeAt), whichever body it lands in.
        const float     outerTexel = desc.TexelCm( Water::kFoamLevels - 1u );
        const glm::vec2 outerMin   = glm::vec2( windows[Water::kFoamLevels - 1u] ) * outerTexel;
        const glm::vec2 outerMax   = outerMin + static_cast<float>( side ) * outerTexel;
        for ( const std::shared_ptr<const Water::RiverFlowField>& river : rivers )
            for ( const Water::RiverFall& fall : river->Falls )
            {
                const Water::FallPlunge& plunge = fall.Plunge;
                const glm::vec2          foot( plunge.Foot.x, plunge.Foot.z );
                if ( glm::any( glm::lessThan( foot + plunge.Radius, outerMin ) ) ||
                     glm::any( glm::greaterThan( foot - plunge.Radius, outerMax ) ) )
                    continue;
                frame.Plunges.push_back( FoamPlunge{
                     .FootRadius = glm::vec4( plunge.Foot, plunge.Radius ),
                     .AcrossHalf = glm::vec4( plunge.Across.x, plunge.Across.z, plunge.HalfWidth, plunge.OutflowSpeed ),
                     .Landing    = glm::vec4( plunge.Downstream, plunge.LandingSpeed, 0.0f ) } );
            }
        m_FoamFrame = std::move( frame );
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
        data.Time = glm::vec4( timeSeconds, held ? age : 0.0f, held ? 1.0f : 0.0f,
                               static_cast<float>( frame.Bodies.size() ) );
        if ( m_FoamChanged )
            data.Changed = *m_FoamChanged;
        data.PlungeCount = glm::vec4( static_cast<float>( frame.Plunges.size() ), 0.0f, 0.0f, 0.0f );

        const RDG::BufferRef uniforms = graph.CreateBuffer( RDG::BufferDesc{ sizeof( data ) }, "Water.FoamUB" );
        graph.QueueBufferUpload( uniforms, std::as_bytes( std::span<const WaterFoamUBData>( &data, 1 ) ) );
        const RDG::BufferRef tiles = graph.CreateBuffer( RDG::BufferDesc{ waves.size_bytes() }, "Water.FoamWaves" );
        graph.QueueBufferUpload( tiles, waves );
        const std::span<const std::byte> bodyBytes = std::as_bytes( std::span( frame.Bodies ) );
        const RDG::BufferRef             bodies =
             graph.CreateBuffer( RDG::BufferDesc{ bodyBytes.size_bytes() }, "Water.FoamBodies" );
        graph.QueueBufferUpload( bodies, bodyBytes );
        // A frame with no plunge binds one zero row (a storage buffer is never empty); PlungeCount.x says 0.
        static constexpr FoamPlunge      kNoPlunge{};
        const std::span<const std::byte> plungeBytes =
             frame.Plunges.empty() ? std::as_bytes( std::span<const FoamPlunge>( &kNoPlunge, 1 ) )
                                   : std::as_bytes( std::span( frame.Plunges ) );
        const RDG::BufferRef plunges =
             graph.CreateBuffer( RDG::BufferDesc{ plungeBytes.size_bytes() }, "Water.FoamPlunges" );
        graph.QueueBufferUpload( plunges, plungeBytes );

        using P = WaterFoamShaderParameters;
        graph.AddPass(
             "Water: Foam", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.ColorTarget( 0, imports.FoamTarget, RDG::LoadOp::DontCare() );
                 pass.Bindings( m_FoamLayout.Get( m_FoamPipeline->GetSpecification().Shader ), RDG::NoOtherRouteFill() )
                      .Storage( P::WaterTiles, tiles, RDG::Access::StorageRead )
                      .Storage( P::WaterFoamBodies, bodies, RDG::Access::StorageRead )
                      .Storage( P::WaterFoamPlunges, plunges, RDG::Access::StorageRead )
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

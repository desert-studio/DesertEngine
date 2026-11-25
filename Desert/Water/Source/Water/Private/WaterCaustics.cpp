#include <Water/Public/WaterCaustics.hpp>

#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGroupSize = 8; // both shaders' LocalSize(8, 8, 1)

        // THE MAP'S SQUARE: kMapSize texels of 4 cm = 20.48 m (WindowCentre). A caustic line is a few cm wide
        // on a shallow floor (Wallace's pool: the focal lines a quarter of a ripple wavelength apart), so a
        // texel must be no wider than the narrowest line the waves draw; past the square the receiver blends to
        // the plain sun over WATER_CAUSTICS_BORDER_UV.
        constexpr float kTexelCm = 4.0f;

        // The sun's travel must point down by at least this much: a sun at the horizon refracts at the critical
        // angle and lights no floor.
        constexpr float kMinSunDescent = 0.01f;

        // THE SQUARE'S CENTRE: the point of the body nearest the camera (in X, Z; the smaller height over its
        // still level breaks a tie) closest to the camera - the camera itself when it stands over that body, so
        // the lines are drawn where the view is, and the body's nearest shore when it does not, so a camera on
        // the bank looking at a pool still has the pool's floor in the map.
        glm::vec2 WindowCentre( const std::span<const Water::WaterQuadTreeBody> bodies, const glm::vec3& camera )
        {
            const glm::vec2 eye( camera.x, camera.z );
            glm::vec2       best          = eye;
            float           bestDistance  = std::numeric_limits<float>::max();
            float           bestElevation = std::numeric_limits<float>::max();
            for ( const Water::WaterQuadTreeBody& body : bodies )
            {
                const glm::vec2 closest   = glm::clamp( eye, body.FootprintMin, body.FootprintMax );
                const float     distance  = glm::length( closest - eye );
                const float     elevation = std::abs( camera.y - body.SurfaceY );
                if ( distance < bestDistance || ( distance == bestDistance && elevation < bestElevation ) )
                {
                    best          = closest;
                    bestDistance  = distance;
                    bestElevation = elevation;
                }
            }
            return best;
        }

        constexpr uint32_t GroupCount( const uint32_t size )
        {
            return ( size + kGroupSize - 1 ) / kGroupSize;
        }

        std::shared_ptr<ComputePipeline> MakePipeline( const std::string& name )
        {
            auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
            if ( shaders == nullptr )
            {
                LOG_ERROR( "[Water] caustics: no shader service for '{}'", name );
                return nullptr;
            }
            const auto shader = shaders->GetByName( name );
            if ( !shader )
            {
                LOG_ERROR( "[Water] caustics: missing compute shader '{}'", name );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !built )
            {
                LOG_ERROR( "[Water] caustics: '{}' does not build: {}", name, built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        }
    } // namespace

    Common::BoolResultStr WaterCaustics::Initialize()
    {
        m_SplatPipeline   = MakePipeline( "WaterCausticsSplat" );
        m_ResolvePipeline = MakePipeline( "WaterCausticsResolve" );
        if ( !m_SplatPipeline || !m_ResolvePipeline )
        {
            m_SplatPipeline.reset();
            m_ResolvePipeline.reset();
            return Common::MakeError( std::string( "WaterCaustics: the caustics programs did not build" ) );
        }
        if ( const auto valid = m_SplatLayout.Validate( m_SplatPipeline->GetSpecification().Shader,
                                                        WaterCausticsSplatShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_ResolveLayout.Validate( m_ResolvePipeline->GetSpecification().Shader,
                                                          WaterCausticsResolveShaderParameters::kAll );
             !valid )
            return valid;
        return BOOLSUCCESS;
    }

    void WaterCaustics::ReportState( const bool active, const std::string_view why ) noexcept
    {
        if ( m_Reported == active )
            return;
        m_Reported = active;
        if ( active )
        {
            LOG_INFO( "[Water] caustics=on res={}", kMapSize );
        }
        else
        {
            LOG_INFO( "[Water] caustics=off ({})", why );
        }
    }

    // The graph's 'off' row (FrameTextures::SetSceneViewInputsOff) is this head, zeroed.
    static_assert( sizeof( WaterCausticsFrameHead ) == kSceneViewWaterCausticsHeadBytes,
                   "the 'off' row is one zeroed WaterCausticsFrameHead" );

    std::optional<WaterCausticsFrame> WaterCaustics::AddPasses( RDG::Builder& graph, const FrameInputs& inputs )
    {
        if ( !m_Enabled )
        {
            ReportState( false, "Effects.WaterCaustics 0" );
            return std::nullopt;
        }
        if ( !m_SplatPipeline || !m_ResolvePipeline )
        {
            ReportState( false, "the caustics programs did not build" );
            return std::nullopt;
        }
        if ( inputs.Waves.empty() || inputs.Bodies.empty() || !inputs.Water.IsValid() )
        {
            ReportState( false, "no water body draws" );
            return std::nullopt;
        }
        if ( inputs.BodyWaveLanes.size() != inputs.Bodies.size() || inputs.BodyCaustics.size() != inputs.Bodies.size() )
        {
            // WaterRenderer fills them in one step (one of each per body drawn): a mismatch is a caller's bug.
            ReportState( false, "the bodies' wave lanes and caustics do not match the bodies" );
            return std::nullopt;
        }
        const glm::vec3 sun = glm::normalize( inputs.SunTravel );
        if ( !( sun.y < -kMinSunDescent ) )
        {
            ReportState( false, "the sun is below the horizon" );
            return std::nullopt;
        }
        // The square (WindowCentre), its lower corner on the texel grid so the map does not swim as it moves.
        const float     side  = static_cast<float>( kMapSize ) * kTexelCm;
        const glm::vec2 lower = glm::floor( ( WindowCentre( inputs.Bodies, inputs.Camera ) - 0.5f * side ) / kTexelCm ) *
                                kTexelCm;
        const glm::vec2 upper = lower + side;

        // THE FRAME ROW: the head, then every body whose footprint meets the square (a receiver outside them all
        // takes no caustics). Each keeps its own still level: the splat starts a texel's rays at the level of
        // the body over it, and a receiver measures its depth under the body over it.
        std::vector<WaterCausticsBodyRow> rows;
        rows.reserve( inputs.Bodies.size() );
        for ( std::size_t i = 0; i < inputs.Bodies.size(); ++i )
        {
            const Water::WaterQuadTreeBody& body = inputs.Bodies[i];
            if ( body.FootprintMax.x >= lower.x && body.FootprintMin.x <= upper.x && body.FootprintMax.y >= lower.y &&
                 body.FootprintMin.y <= upper.y )
                rows.push_back(
                     WaterCausticsBodyRow{ .Footprint = glm::vec4( body.FootprintMin, body.FootprintMax ),
                                           .Level = glm::vec4( body.SurfaceY, inputs.BodyWaveLanes[i],
                                                              body.Ribbon ? 0.0f : 1.0f, 0.0f ),
                                           .Look  = glm::vec4( inputs.BodyCaustics[i], 0.0f ) } );
        }
        if ( rows.empty() )
        {
            // Unreachable while WindowCentre lies on a body's footprint; said rather than drawn blind.
            ReportState( false, "no body meets the caustics square" );
            return std::nullopt;
        }
        ReportState( true, {} );
        const WaterCausticsFrameHead head{
             .Window = glm::vec4( lower, 1.0f / side, 1.0f ),
             .Shape  = glm::vec4( static_cast<float>( rows.size() ), 0.0f, 0.0f, 0.0f ),
        };
        std::vector<std::byte> rowBytes( sizeof( head ) + rows.size() * sizeof( WaterCausticsBodyRow ) );
        std::memcpy( rowBytes.data(), &head, sizeof( head ) );
        std::memcpy( rowBytes.data() + sizeof( head ), rows.data(), rows.size() * sizeof( WaterCausticsBodyRow ) );
        const RDG::BufferRef frame =
             graph.CreateBuffer( RDG::BufferDesc{ rowBytes.size() }, "Water.Caustics.Frame" );
        graph.QueueBufferUpload( frame, rowBytes );

        // The waves block, as the water mesh's WaterTiles head (the shader declares the tile rows after it and
        // reads none).
        const RDG::BufferRef waves =
             graph.CreateBuffer( RDG::BufferDesc{ inputs.Waves.size() }, "Water.Caustics.Waves" );
        graph.QueueBufferUpload( waves, inputs.Waves );

        const uint64_t       energyBytes = uint64_t{ kMapSize } * kMapSize * sizeof( uint32_t );
        const RDG::BufferRef energy =
             graph.CreateBuffer( RDG::BufferDesc{ energyBytes }, "Water.Caustics.Energy" );
        graph.QueueBufferClear( energy, 0u );

        const RDG::TextureRef map =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = kMapSize, .Height = kMapSize },
                                                    .Format = Core::Formats::ImageFormat::R32F },
                                  "Water.Caustics" );
        const ShaderPush::WaterCausticsSplat push{
             .Window = glm::vec4( lower, kTexelCm, 0.0f ),
             .Sun    = glm::vec4( sun, inputs.TimeSeconds ),
        };
        graph.AddPass(
             "Water: Caustics Splat", RDG::PassFlags::Compute,
             [this, waves, water = inputs.Water, energy, frame]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_SplatLayout.Get( m_SplatPipeline->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_SplatPipeline ) )
                      .Storage( WaterCausticsSplatShaderParameters::WaterTiles, waves, RDG::Access::StorageRead )
                      .Texture( WaterCausticsSplatShaderParameters::WaterShoreTexture, water.Shore,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Texture( WaterCausticsSplatShaderParameters::WaterWavesTexture, water.Waves,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( WaterCausticsSplatShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                      .Storage( WaterCausticsSplatShaderParameters::WaterCausticsEnergy, energy,
                                RDG::Access::StorageWrite )
                      .Storage( WaterCausticsSplatShaderParameters::WaterCausticsFrameUB, frame,
                                RDG::Access::StorageRead )
                      .PushConstants<ShaderPush::WaterCausticsSplat>();
             },
             [this, push]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( push );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *m_SplatPipeline, GroupCount( kMapSize ), GroupCount( kMapSize ), 1 );
             } );
        graph.AddPass(
             "Water: Caustics Resolve", RDG::PassFlags::Compute,
             [this, energy, map]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_ResolveLayout.Get( m_ResolvePipeline->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_ResolvePipeline ) )
                      .Storage( WaterCausticsResolveShaderParameters::WaterCausticsEnergy, energy,
                                RDG::Access::StorageRead )
                      .Storage( WaterCausticsResolveShaderParameters::WaterCausticsImage, map,
                                RDG::Access::StorageWrite, 0 );
             },
             [this]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *m_ResolvePipeline, GroupCount( kMapSize ), GroupCount( kMapSize ), 1 );
             } );
        return WaterCausticsFrame{ .Map = map, .Row = frame };
    }
} // namespace Desert::Graphic::System

#include "WaterCaustics.hpp"

#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <cmath>
#include <span>
#include <string>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGroupSize = 8; // both shaders' LocalSize(8, 8, 1)

        // THE MAP'S SQUARE: kMapSize texels of 4 cm = 20.48 m around the camera. A caustic line is a few cm wide
        // on a shallow floor (Wallace's pool: the focal lines a quarter of a ripple wavelength apart), so a
        // texel must be no wider than the narrowest line the waves draw; past the square the receiver blends to
        // the plain sun over WATER_CAUSTICS_BORDER_UV.
        constexpr float kTexelCm = 4.0f;

        // THE LOOK UNDER THE SURFACE (receiver, Common/WaterCaustics.glslh WaterCausticsShape). The map holds the
        // physical area ratio; these shape how a receiver takes it:
        // - onset: the refracted beams need some depth to cross before they focus, and the waterline of a wall
        //   or a rock must not cut a hard caustic edge at the still level - the map comes in over the first
        //   30 cm;
        // - fade: the sun's disc (0.53 deg) and the forward scattering of the water widen every focal line with
        //   depth until the pattern is gone; the contrast falls as exp(-depth / 6 m);
        // - strength 1: the map as computed (no gain on the physics).
        // REMAINDER WATER-CAUSTICS: these three move to the water material (.demat, WaterSurface.shader's
        // properties) once WATER-RIVERS-FFT has landed its edits to that template.
        constexpr float kOnsetDepthCm = 30.0f;
        constexpr float kFadeDepthCm  = 600.0f;
        constexpr float kStrength     = 1.0f;

        // The floor's depth where the body has no WaterInfo ground baked (open sea over no landscape): the plane
        // the beams are followed to. A floor deeper than ~2 m shows the pattern washed out by the fade anyway.
        constexpr float kNoGroundFloorDepthCm = 200.0f;

        // The sun's travel must point down by at least this much: a sun at the horizon refracts at the critical
        // angle and lights no floor.
        constexpr float kMinSunDescent = 0.01f;

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
            LOG_INFO( "[Water] caustics=on res={}", kMapSize );
        else
            LOG_INFO( "[Water] caustics=off ({})", why );
    }

    WaterCausticsFrame WaterCaustics::Off( RDG::Builder& graph, const RDG::TextureRef white )
    {
        const WaterCausticsFrameRow row{};
        const RDG::BufferRef        buffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( row ) }, "Water.Caustics.Frame" );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span( &row, 1 ) ) );
        return WaterCausticsFrame{ .Map = white, .Row = buffer };
    }

    WaterCausticsFrame WaterCaustics::AddPasses( RDG::Builder& graph, const FrameInputs& inputs )
    {
        if ( !m_Enabled )
        {
            ReportState( false, "Effects.WaterCaustics 0" );
            return Off( graph, inputs.White );
        }
        if ( !m_SplatPipeline || !m_ResolvePipeline )
        {
            ReportState( false, "the caustics programs did not build" );
            return Off( graph, inputs.White );
        }
        if ( inputs.Waves.empty() || !inputs.Body )
        {
            ReportState( false, "no water body draws" );
            return Off( graph, inputs.White );
        }
        const glm::vec3 sun = glm::normalize( inputs.SunTravel );
        if ( !( sun.y < -kMinSunDescent ) )
        {
            ReportState( false, "the sun is below the horizon" );
            return Off( graph, inputs.White );
        }
        ReportState( true, {} );

        // The square around the camera, its lower corner on the texel grid so the map does not swim as the
        // camera moves.
        const float     side = static_cast<float>( kMapSize ) * kTexelCm;
        const glm::vec2 lower =
             glm::floor( ( glm::vec2( inputs.Camera.x, inputs.Camera.z ) - 0.5f * side ) / kTexelCm ) * kTexelCm;
        const Water::WaterQuadTreeBody& body = *inputs.Body;

        const WaterCausticsFrameRow row{
             .Window = glm::vec4( lower, 1.0f / side, 1.0f ),
             .Body   = glm::vec4( body.FootprintMin, body.FootprintMax ),
             .Shape  = glm::vec4( body.SurfaceY, kOnsetDepthCm, kFadeDepthCm, kStrength ),
        };
        const RDG::BufferRef frame =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( row ) }, "Water.Caustics.Frame" );
        graph.QueueBufferUpload( frame, std::as_bytes( std::span( &row, 1 ) ) );

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
        const RDG::TextureRef ground = inputs.WaterInfo.IsValid() ? inputs.WaterInfo : inputs.Black;

        const ShaderPush::WaterCausticsSplat push{
             .Window  = glm::vec4( lower, kTexelCm, 0.0f ),
             .Sun     = glm::vec4( sun, inputs.TimeSeconds ),
             .Surface = glm::vec4( body.SurfaceY, kNoGroundFloorDepthCm, 0.0f, 0.0f ),
        };
        graph.AddPass(
             "Water: Caustics Splat", RDG::PassFlags::Compute,
             [this, waves, ground, energy]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_SplatLayout.Get( m_SplatPipeline->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_SplatPipeline ) )
                      .Storage( WaterCausticsSplatShaderParameters::WaterTiles, waves, RDG::Access::StorageRead )
                      .Texture( WaterCausticsSplatShaderParameters::WaterInfoTexture, ground,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Storage( WaterCausticsSplatShaderParameters::WaterCausticsEnergy, energy,
                                RDG::Access::StorageWrite )
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

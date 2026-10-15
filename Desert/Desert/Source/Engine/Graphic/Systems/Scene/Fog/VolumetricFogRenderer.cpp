#include "VolumetricFogRenderer.hpp"

#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Fog/FogPayload.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RenderConfig.hpp> // GlobalTextureFilterSampler, VolumeSampler
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kScatteringShaderName  = "VolumetricFogLightScattering";
        constexpr const char* kIntegrationShaderName = "VolumetricFogIntegration";

        // LocalSize of the two shaders: 4x4x4 per froxel, 8x8 per column.
        constexpr uint32_t kScatteringGroup  = 4;
        constexpr uint32_t kIntegrationGroup = 8;

        // UE jitters the froxel sample by Halton(2,3,5) over 16 frames (r.VolumetricFog.Jitter).
        constexpr uint32_t kJitterSequenceLength = 16;

        constexpr uint32_t Groups( uint32_t extent, uint32_t group )
        {
            return ( extent + group - 1 ) / group;
        }

        // Radical inverse of @p index (>= 1) in @p base: the Halton sequence's value there, in (0, 1).
        float Halton( uint32_t index, uint32_t base )
        {
            float result   = 0.0f;
            float fraction = 1.0f / static_cast<float>( base );
            while ( index > 0 )
            {
                result += static_cast<float>( index % base ) * fraction;
                index /= base;
                fraction /= static_cast<float>( base );
            }
            return result;
        }

        // One world unit is a centimetre; UE's FogDensity / FogHeightFalloff are "per 1000 cm" in the exp2 form
        // the closed form integrates (FogPayload.hpp), so per centimetre they are the authored value / 1000.
        constexpr float kUEUnitsPerCm = 1.0f / 1000.0f;
    } // namespace

    VolumetricFogRenderer::~VolumetricFogRenderer() = default;

    Common::BoolResultStr VolumetricFogRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError(
                 std::format( "VolumetricFogRenderer: the shaders could not be resolved ({} / {})",
                              kScatteringShaderName, kIntegrationShaderName ) );

        // Non-persistent: one copy per (frame x recording renderer slot), Docs/RENDERER_FRAME_STATE.md.
        m_ParamsBuffer = ShaderResources::StorageBuffer::Create(
             "VolumetricFogParams", static_cast<uint32_t>( sizeof( VolumetricFogGpuPayload ) ),
             kVolumetricFogParamsBinding, /*persistent=*/false );
        if ( !m_ParamsBuffer )
            return Common::MakeError( "VolumetricFogRenderer: could not create the parameter buffer" );
        return BOOLSUCCESS;
    }

    bool VolumetricFogRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return false;

        const auto create = [&]( const char* name, std::shared_ptr<ComputePipeline>& out ) -> bool
        {
            const auto shader = shaderService->GetByName( name );
            if ( !shader )
            {
                LOG_ERROR( "[VolumetricFog] Compute shader '{}' is not registered. Expected "
                           "Editor/Resources/Shaders/Programs/Fog/{}.shader.",
                           name, name );
                return false;
            }
            const auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !pipeline )
            {
                LOG_ERROR( "[VolumetricFog] {}: {}", name, pipeline.GetError() );
                return false;
            }
            out = pipeline.GetValue();
            return true;
        };
        return create( kScatteringShaderName, m_ScatteringPipeline ) &&
               create( kIntegrationShaderName, m_IntegrationPipeline );
    }

    void VolumetricFogRenderer::SetFogSettings( bool present, const ECS::ExponentialHeightFogData& data,
                                                float fogHeightY )
    {
        m_Present    = present;
        m_Data       = data;
        m_FogHeightY = fogHeightY;
    }

    void VolumetricFogRenderer::SetQuality( const VolumetricFogQuality& quality )
    {
        m_Quality = quality;
    }

    bool VolumetricFogRenderer::IsActive() const
    {
        return m_Present && m_Data.Enabled && m_Data.VolumetricFog && m_Quality.Enabled && m_ScatteringPipeline &&
               m_IntegrationPipeline && m_ParamsBuffer;
    }

    bool VolumetricFogRenderer::EnsureVolumes( const glm::uvec3& gridSize )
    {
        if ( m_VolumeSize == gridSize && m_Integrated && m_LightScattering[0] && m_LightScattering[1] )
            return true;

        const auto make = [&gridSize]( const char* tag ) -> std::shared_ptr<Image3D>
        {
            const Core::Formats::Image3DSpecification spec{
                 .Tag        = tag,
                 .Width      = gridSize.x,
                 .Height     = gridSize.y,
                 .Depth      = gridSize.z,
                 .Format     = Core::Formats::ImageFormat::RGBA16F,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            return Image3D::Create( spec );
        };
        m_LightScattering[0] = make( "VolumetricFogLightScatteringA" );
        m_LightScattering[1] = make( "VolumetricFogLightScatteringB" );
        m_Integrated         = make( "VolumetricFogIntegrated" );
        if ( !m_LightScattering[0] || !m_LightScattering[1] || !m_Integrated )
        {
            LOG_ERROR( "[VolumetricFog] the froxel volumes ({}x{}x{} RGBA16F) could not be created; the fog stays "
                       "analytic this frame",
                       gridSize.x, gridSize.y, gridSize.z );
            m_LightScattering = {};
            m_Integrated      = nullptr;
            m_VolumeSize      = glm::uvec3( 0u );
            return false;
        }
        // New volumes hold nothing of the last frame.
        m_VolumeSize = gridSize;
        m_History.Invalidate();
        return true;
    }

    std::vector<ComputeNodeDeclaration>
    VolumetricFogRenderer::DeclareFrameNodes( const FrameGraphRefs& refs, const ViewFrame& frame,
                                              const VolumetricFogSunInput& sun )
    {
        std::vector<ComputeNodeDeclaration> nodes;
        m_Result = VolumetricFogFrameResult{};
        if ( !IsActive() )
            return nodes;

        const FrameTransients& t = refs.Transients;
        if ( !t.Lights.Point.IsValid() || !t.Lights.Spot.IsValid() || !t.Lights.ShadowViews.IsValid() ||
             !t.Lights.GridParams.IsValid() || !t.Lights.GridCells.IsValid() || !t.ShadowCascades.IsValid() )
        {
            LOG_ERROR(
                 "[VolumetricFog] the view has no light grid or no cascade array this frame (AddFrameLightGrid "
                 "/ ImportSceneViewTextures did not run): the fog stays analytic" );
            return nodes;
        }

        const uint32_t width  = frame.Split.Render.Width;
        const uint32_t height = frame.Split.Render.Height;
        if ( width == 0 || height == 0 )
            return nodes;

        const glm::uvec3 grid = MakeVolumetricFogGridSize( width, height, m_Quality );
        if ( !EnsureVolumes( grid ) )
            return nodes;

        const float     distance = std::max( m_Data.VolumetricFogDistance, 1000.0f );
        const glm::vec4 zParams  = MakeVolumetricFogGridZParams( frame.NearPlane, distance, grid.z );

        // The history is last frame's of THIS view (a new grid recreated the volumes and dropped it), and only
        // if the view did not reset it - otherwise the reprojection would blend in a different picture.
        const bool historyValid = m_History.ReadableIn( frame );
        const uint32_t current = historyValid ? 1u - m_Current : m_Current;
        const uint32_t history = 1u - current;

        const AtmosphereEnv& atmosphere = m_SceneRenderer->GetAtmosphere();
        const bool           physical   = atmosphere.Valid && atmosphere.DistantSkyLight != nullptr;

        VolumetricFogGpuPayload p{};
        p.InvViewProjection  = frame.InvViewProjection;
        p.ViewProjection     = frame.ViewProjection;
        p.PrevViewProjection = frame.PrevViewProjection;
        for ( uint32_t c = 0; c < 4; ++c )
            p.LightViewProj[c] = sun.CascadeViewProj[c];
        const uint32_t jitterIndex = static_cast<uint32_t>( frame.FrameIndex % kJitterSequenceLength ) + 1u;
        p.GridSize                 = glm::uvec4( grid, jitterIndex );
        p.GridZParams              = zParams;
        p.GridPixel =
             glm::vec4( static_cast<float>( std::max( m_Quality.GridPixels, 1u ) ), static_cast<float>( width ),
                        static_cast<float>( height ), historyValid ? 1.0f : 0.0f );
        p.Jitter        = glm::vec4( Halton( jitterIndex, 2 ), Halton( jitterIndex, 3 ), Halton( jitterIndex, 5 ),
                                     kVolumetricFogHistoryWeight );
        p.Camera        = glm::vec4( frame.CameraPosition,
                                     glm::clamp( m_Data.VolumetricFogScatteringDistribution, -0.9f, 0.9f ) );
        p.CameraForward = glm::vec4( FogCameraForward( frame.View ), 0.0f );
        p.PrevCamera    = glm::vec4( frame.PrevCameraPosition, 0.0f );
        p.PrevCameraForward = glm::vec4( FogCameraForward( frame.PrevView ), 0.0f );
        p.Albedo       = glm::vec4( glm::clamp( m_Data.VolumetricFogAlbedo, glm::vec3( 0.0f ), glm::vec3( 1.0f ) ),
                                    glm::max( m_Data.VolumetricFogExtinctionScale, 0.0f ) );
        p.Emissive     = glm::vec4( glm::max( m_Data.VolumetricFogEmissive, glm::vec3( 0.0f ) ),
                                    glm::max( m_Data.VolumetricFogStartDistance, 0.0f ) );
        p.Fade         = glm::vec4( glm::max( m_Data.VolumetricFogNearFadeInDistance, 0.0f ), 0.0f, 0.0f, 0.0f );
        p.Layer0       = glm::vec4( glm::max( m_Data.FogDensity, 0.0f ) * kUEUnitsPerCm,
                                    glm::max( m_Data.FogHeightFalloff, 0.0f ) * kUEUnitsPerCm, m_FogHeightY, 0.0f );
        p.Layer1       = glm::vec4( glm::max( m_Data.SecondFogDensity, 0.0f ) * kUEUnitsPerCm,
                                    glm::max( m_Data.SecondFogHeightFalloff, 0.0f ) * kUEUnitsPerCm,
                                    m_FogHeightY + m_Data.SecondFogHeightOffset, 0.0f );
        p.SunDirection = glm::vec4( sun.TowardSun, sun.Present ? 1.0f : 0.0f );
        p.SunRadiance  = glm::vec4( sun.Radiance, static_cast<float>( std::min( sun.CascadeCount, 4u ) ) );
        // A cascade's comparison bias in its map's depth units: one and a half texels of world size over the
        // world length the map's [0,1] depth spans - the receiver is a point of fog, not a surface, so there is no
        // normal to offset along and no slope to scale by.
        for ( int c = 0; c < 4; ++c )
            p.CascadeBias[c] = 1.5f * sun.CascadeWorldPerTexel[c] / std::max( sun.CascadeDepthRange[c], 1e-3f );
        // The sky: the same two models the height fog's ambient follows (PackFogParams). Physical: the distant
        // sky light texel, scaled; artistic: the dome's mean, folded here.
        if ( physical )
        {
            p.SkyAmbient      = glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f );
            p.SkyAmbientScale = glm::vec4( m_Data.SkyAtmosphereAmbientContributionColorScale, 0.0f );
        }
        else if ( atmosphere.Valid )
        {
            p.SkyAmbient = glm::vec4( m_Data.SkyAtmosphereAmbientContributionColorScale * 0.5f *
                                           ( atmosphere.ZenithRadiance + atmosphere.GroundRadiance ),
                                      0.0f );
        }

        if ( const auto uploaded = m_ParamsBuffer->SetData( &p, static_cast<uint32_t>( sizeof( p ) ) );
             !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[VolumetricFog] the volumetric fog does not run this frame, its parameters were not "
                       "uploaded: {}",
                       uploaded.GetError() );
            return nodes;
        }
        m_ScatteringPipeline->SetStorageBuffer( kVolumetricFogParamsBinding, m_ParamsBuffer.get() );
        m_IntegrationPipeline->SetStorageBuffer( kVolumetricFogParamsBinding, m_ParamsBuffer.get() );

        const std::shared_ptr<Image> distantSkyLight =
             atmosphere.DistantSkyLight
                  ? std::shared_ptr<Image>( atmosphere.DistantSkyLight )
                  : FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F );
        const RDG::TextureRef atlas = t.LocalShadowAtlas.IsValid() ? t.LocalShadowAtlas : refs.System.White;

        const std::string currentName =
             current == 0 ? "VolumetricFog.LightScatteringA" : "VolumetricFog.LightScatteringB";
        // On a reset (first frame, scene swap, camera cut, grid change: PassHistoryStamp says it) the pass does
        // not read the other volume at all - no graph read of a never-written or stale image is declared; the
        // slot holds the engine's 3D fallback (a declared sampler needs an image) and GridPixel.w = 0 says the
        // shader writes this frame's result only.
        const std::string historyName =
             history == 0 ? "VolumetricFog.LightScatteringA" : "VolumetricFog.LightScatteringB";

        std::shared_ptr<Image> historyImage =
             FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F );
        std::string historyImageName = "VolumetricFog.HistoryReset";
        if ( historyValid )
        {
            historyImage     = std::shared_ptr<Image>( m_LightScattering[history] );
            historyImageName = historyName;
        }

        ComputeNodeDeclaration scattering;
        scattering.Name = "VolumetricFog: LightScattering";
        scattering.Access
             .Bindings( m_ScatteringLayout.Get( m_ScatteringPipeline->GetShader() ),
                        Renderer::GetPipelineRouteFill( *m_ScatteringPipeline ) )
             .Storage( "u_LightScattering", std::shared_ptr<Image>( m_LightScattering[current] ),
                       RDG::Access::StorageWrite, currentName )
             .Sampled( "u_LightScatteringHistory", historyImage, RDG::Access::SampledCompute, VolumeSampler(),
                       historyImageName )
             // Read with textureGather (raw depths), so the filter mode is never used.
             .Sampled( kSceneViewShadowCascadesName, t.ShadowCascades, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() )
             .Sampled( kSceneViewLocalShadowAtlasName, atlas, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
             .Sampled( "u_DistantSkyLight", distantSkyLight, RDG::Access::SampledCompute,
                       GlobalTextureFilterSampler(),
                       atmosphere.DistantSkyLight ? "Sky.DistantLight" : "VolumetricFog.DistantSkyLightFallback" )
             .Storage( ShaderProtocols::PointLight::Name, t.Lights.Point, RDG::Access::StorageRead )
             .Storage( ShaderProtocols::SpotLight::Name, t.Lights.Spot, RDG::Access::StorageRead )
             .Storage( "LocalShadowViewsUB", t.Lights.ShadowViews, RDG::Access::StorageRead )
             .Storage( kLightGridParamsBlockName, t.Lights.GridParams, RDG::Access::StorageRead )
             .Storage( kLightGridCellsBlockName, t.Lights.GridCells, RDG::Access::StorageRead );
        scattering.Record = [this, grid, current, frameIndex = frame.FrameIndex](
                                 RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            const RDG::PassBindings     bindings( context, context.GetBindingBlock( 0 ) );
            const Common::BoolResultStr dispatched = Renderer::DispatchCompute(
                 bindings, *m_ScatteringPipeline, Groups( grid.x, kScatteringGroup ),
                 Groups( grid.y, kScatteringGroup ), Groups( grid.z, kScatteringGroup ) );
            if ( dispatched )
            {
                // Recorded: the view's next frame may reproject from this volume.
                m_Current = current;
                m_History.Stamp( frameIndex );
            }
            return dispatched;
        };
        nodes.push_back( std::move( scattering ) );

        ComputeNodeDeclaration integration;
        integration.Name = "VolumetricFog: Integration";
        integration.Access
             .Bindings( m_IntegrationLayout.Get( m_IntegrationPipeline->GetShader() ),
                        Renderer::GetPipelineRouteFill( *m_IntegrationPipeline ) )
             .Storage( "u_IntegratedLightScattering", std::shared_ptr<Image>( m_Integrated ),
                       RDG::Access::StorageWrite, "VolumetricFog.Integrated" )
             .Sampled( "u_LightScattering", std::shared_ptr<Image>( m_LightScattering[current] ),
                       RDG::Access::SampledCompute, VolumeSampler(), currentName );
        integration.Record = [this, grid]( RDG::PassContext& context,
                                           const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::DispatchCompute( bindings, *m_IntegrationPipeline,
                                              Groups( grid.x, kIntegrationGroup ),
                                              Groups( grid.y, kIntegrationGroup ), 1 );
        };
        nodes.push_back( std::move( integration ) );

        m_Result.Integrated  = m_Integrated;
        m_Result.GridSize    = grid;
        m_Result.GridZParams = zParams;
        m_Result.GridPixels  = static_cast<float>( std::max( m_Quality.GridPixels, 1u ) );
        m_Result.Distance    = distance;
        return nodes;
    }
} // namespace Desert::Graphic::System

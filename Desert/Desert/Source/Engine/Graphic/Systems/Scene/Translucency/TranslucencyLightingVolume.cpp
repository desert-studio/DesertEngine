#include "TranslucencyLightingVolume.hpp"

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kShaderName = "TranslucencyLightingVolume";
        // LocalSize of the fill: 4x4x4 cells.
        constexpr uint32_t kGroup = 4;
        // Common/TranslucencyLightingVolume.glslh TLV_CASCADES: the cascades stacked along z.
        constexpr uint32_t kCascades = 2;
    } // namespace

    TranslucencyLightingVolumeRenderer::~TranslucencyLightingVolumeRenderer() = default;

    Core::ShaderVariant TranslucencyLightingVolumeRenderer::VsmVariant()
    {
        return Core::ShaderVariant{ .Defines = { std::string( kLocalShadowVsmDefine ) }, .ShippedByCook = true };
    }

    std::vector<Core::CookedProgramVariant> TranslucencyLightingVolumeRenderer::ProgramVariants()
    {
        return { { std::string( kShaderName ), VsmVariant() } };
    }

    Common::BoolResultStr TranslucencyLightingVolumeRenderer::Initialize()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return Common::MakeError( std::string( "TranslucencyLightingVolumeRenderer: no shader service" ) );
        const auto shader = shaderService->GetByName( kShaderName );
        if ( !shader )
            return Common::MakeError(
                 std::format( "TranslucencyLightingVolumeRenderer: compute shader '{}' is not "
                              "registered (Programs/Translucency/{}.shader)",
                              kShaderName, kShaderName ) );
        const auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = kShaderName } );
        if ( !pipeline )
            return Common::MakeError(
                 std::format( "TranslucencyLightingVolumeRenderer: {}", pipeline.GetError() ) );
        if ( const auto valid = m_Layout.Validate( shader, TranslucencyLightingVolumeShaderParameters::kAll );
             !valid )
            return Common::MakeError( std::format( "TranslucencyLightingVolumeRenderer: {}", valid.GetError() ) );
        m_Pipeline = pipeline.GetValue();

        // Non-persistent: one copy per (frame x recording renderer slot), Docs/RENDERER_FRAME_STATE.md.
        m_ParamsBuffer = ShaderResources::StorageBuffer::Create(
             "TranslucencyLightingVolumeParams",
             static_cast<uint32_t>( sizeof( TranslucencyLightingVolumeGpuPayload ) ),
             kTranslucencyLightingVolumeParamsBinding, /*persistent=*/false );
        if ( !m_ParamsBuffer )
            return Common::MakeError(
                 std::string( "TranslucencyLightingVolumeRenderer: could not create the parameter buffer" ) );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr TranslucencyLightingVolumeRenderer::EnsureVsmPipeline()
    {
        if ( m_VsmPipeline )
            return BOOLSUCCESS;
        if ( !m_VsmRefusal.empty() )
            return Common::MakeError( m_VsmRefusal );
        const auto refuse = [this]( std::string why ) -> Common::BoolResultStr
        {
            m_VsmRefusal = std::move( why );
            LOG_ERROR( "[TranslucencyLightingVolume] the volume is not filled under Shadows.Method VirtualMaps "
                       "(lit particles receive no light): {}",
                       m_VsmRefusal );
            return Common::MakeError( m_VsmRefusal );
        };
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return refuse( "no shader service" );
        auto acquired = shaderService->AcquireVariant( kShaderName, VsmVariant() );
        if ( !acquired )
            return refuse( std::format( "{} ({}) was refused: {}", kShaderName, kLocalShadowVsmDefine,
                                        acquired.GetError() ) );
        const auto  shader    = acquired.ExtractValue();
        std::string debugName = std::string( kShaderName ) + "|DESERT_LOCAL_SHADOW_VSM";
        const auto  pipeline  = ComputePipeline::Create( { .Shader = shader, .DebugName = debugName } );
        if ( !pipeline )
            return refuse( pipeline.GetError() );
        if ( const auto valid =
                  m_VsmLayout.Validate( shader, TranslucencyLightingVolumeShaderParameters::kVirtualLocal );
             !valid )
            return refuse( valid.GetError() );
        m_VsmPipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    void TranslucencyLightingVolumeRenderer::SetQuality( const TranslucencyLightingVolumeQuality& quality )
    {
        m_Quality = quality;
    }

    bool TranslucencyLightingVolumeRenderer::EnsureVolume( const uint32_t dim )
    {
        if ( m_Volume && m_VolumeDim == dim )
            return true;
        const Core::Formats::Image3DSpecification spec{
             .Tag        = kVolumeName,
             .Width      = dim,
             .Height     = dim,
             .Depth      = dim * kCascades,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_Volume = Image3D::Create( spec );
        if ( !m_Volume )
        {
            LOG_ERROR( "[TranslucencyLightingVolume] the volume ({}x{}x{} RGBA16F) could not be created; lit "
                       "particles receive no light this frame",
                       dim, dim, dim * kCascades );
            m_VolumeDim = 0;
            return false;
        }
        m_VolumeDim = dim;
        return true;
    }

    ComputeNodeList TranslucencyLightingVolumeRenderer::DeclareFrameNodes( Common::Memory::FrameArena&  arena,
                                                                           const FrameGraphRefs&        refs,
                                                                           const ViewFrame&             frame,
                                                                           const VolumetricFogSunInput& sun,
                                                                           const bool virtualLocalShadows )
    {
        ComputeNodeList nodes = MakeComputeNodeList( arena );
        m_FrameVolume         = nullptr;
        if ( !m_Pipeline )
            return nodes;

        const FrameTransients& t = refs.Transients;
        if ( !t.Lights.Point.IsValid() || !t.Lights.Spot.IsValid() ||
             ( !virtualLocalShadows && !t.Lights.ShadowViews.IsValid() ) || !t.Lights.GridParams.IsValid() ||
             !t.Lights.GridCells.IsValid() || !t.ShadowCascades.IsValid() )
        {
            LOG_ERROR(
                 "[TranslucencyLightingVolume] the view has no light grid or no cascade array this frame "
                 "(AddFrameLightGrid / ImportSceneViewTextures did not run): lit particles receive no light" );
            return nodes;
        }
        if ( virtualLocalShadows )
        {
            if ( !t.Vsm.IsValid() )
            {
                LOG_ERROR( "[TranslucencyLightingVolume] Shadows.Method VirtualMaps and the view built no virtual "
                           "shadow map this frame (AddFrameVirtualShadows): lit particles receive no light" );
                return nodes;
            }
            if ( !EnsureVsmPipeline() )
                return nodes;
        }
        ComputePipeline* const    pipeline = virtualLocalShadows ? m_VsmPipeline.get() : m_Pipeline.get();
        ShaderBindingLayoutCache& layout   = virtualLocalShadows ? m_VsmLayout : m_Layout;

        const uint32_t width  = frame.Split.Render.Width;
        const uint32_t height = frame.Split.Render.Height;
        if ( width == 0 || height == 0 )
            return nodes;
        // Even, so a cascade centres on the camera's cell (Common/TranslucencyLightingVolume.glslh TlvCascadeMin).
        const uint32_t dim = std::max( m_Quality.Dim & ~1u, 2u );
        if ( !EnsureVolume( dim ) )
            return nodes;

        TranslucencyLightingVolumeGpuPayload p{};
        p.ViewProjection = frame.ViewProjection;
        for ( uint32_t c = 0; c < 4; ++c )
            p.CascadeViewProj[c] = sun.CascadeViewProj[c];
        p.Camera       = glm::vec4( frame.CameraPosition, static_cast<float>( dim ) );
        p.RenderSize   = glm::vec4( static_cast<float>( width ), static_cast<float>( height ), 0.0f, 0.0f );
        p.SunDirection = glm::vec4( sun.TowardSun, sun.Present ? 1.0f : 0.0f );
        p.SunRadiance  = glm::vec4( sun.Radiance, static_cast<float>( std::min( sun.CascadeCount, 4u ) ) );
        // The volumetric fog's bias: one and a half texels of world size over the map's depth span (a cell is a
        // point of medium: no normal to offset along).
        for ( int c = 0; c < 4; ++c )
            p.CascadeBias[c] = 1.5f * sun.CascadeWorldPerTexel[c] / std::max( sun.CascadeDepthRange[c], 1e-3f );
        p.CloudShadowWorldToMap   = sun.CloudShadow.WorldToMap;
        p.CloudShadowParams       = sun.CloudShadow.Params;
        p.RainOcclusionWorldToMap = sun.SkyOcclusion.WorldToMap;
        p.RainOcclusionParams     = t.RainOcclusionMap.IsValid() ? sun.SkyOcclusion.Params : glm::vec4( 0.0f );
        if ( const auto uploaded = m_ParamsBuffer->SetData( &p, static_cast<uint32_t>( sizeof( p ) ) );
             !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[TranslucencyLightingVolume] the volume is not filled this frame, its parameters were not "
                       "uploaded: {}",
                       uploaded.GetError() );
            return nodes;
        }
        pipeline->SetStorageBuffer( kTranslucencyLightingVolumeParamsBinding, m_ParamsBuffer.get() );

        // Named here, not as a conditional inside the by-value argument (MSVC 19.44 hangs on that shape).
        RDG::TextureRef rainOcclusionMap = refs.System.White;
        if ( t.RainOcclusionMap.IsValid() )
            rainOcclusionMap = t.RainOcclusionMap;
        // A scene without a baked sky has no irradiance cube: the sky term is black, as the surfaces' ambient.
        RDG::TextureRef skyLight = refs.System.BlackCube;
        if ( t.EnvIrradiance.IsValid() )
            skyLight = t.EnvIrradiance;

        using Fill = TranslucencyLightingVolumeShaderParameters;
        ComputeNodeDeclaration fill( arena );
        fill.Name  = "Translucency: Lighting Volume";
        auto block = fill.Access.Bindings( layout.Get( pipeline->GetShader() ),
                                           Renderer::GetPipelineRouteFill( *pipeline ) );
        block.Storage( Fill::TranslucencyLightingVolumeOutput, std::shared_ptr<Image>( m_Volume ),
                       RDG::Access::StorageWrite, std::string( kVolumeName ) )
             .Sampler( Fill::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             // Read with textureGather (raw depths), so the filter mode is never used.
             .Texture( Fill::ShadowCascadesTexture, t.ShadowCascades, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( Fill::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
             .Texture( Fill::EnvironmentIrradianceTexture, skyLight, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Storage( Fill::PointLightsUB, t.Lights.Point, RDG::Access::StorageRead )
             .Storage( Fill::SpotLightsUB, t.Lights.Spot, RDG::Access::StorageRead )
             .Storage( Fill::LightGridParamsUB, t.Lights.GridParams, RDG::Access::StorageRead )
             .Storage( Fill::LightGridCellsUB, t.Lights.GridCells, RDG::Access::StorageRead )
             // The cloud layer's map (white when none casts: CloudShadowParams.y = 0 and it is never fetched).
             .Texture( Fill::CloudShadowMapTexture, CloudShadowMapOrWhite( refs ), RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             // Read with textureGather (raw depths); white = depth 1, "nothing overhead", when the view has none.
             .Texture( Fill::RainOcclusionMapTexture, rainOcclusionMap, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() );
        if ( virtualLocalShadows )
        {
            block.Storage( kVsmFrameDataBlock, t.Vsm.FrameData, RDG::Access::StorageRead )
                 .Storage( kVsmPageTableBlock, t.Vsm.PageTable, RDG::Access::StorageRead )
                 .Storage( kVsmPhysicalPool, t.Vsm.PhysicalPool, RDG::Access::StorageRead )
                 .Storage( kVsmLocalLightsBlock, t.Vsm.LocalLights, RDG::Access::StorageRead );
        }
        else
        {
            // A frame with no casting local light draws no atlas: its views table is empty, the slot white.
            const RDG::TextureRef atlas = t.LocalShadowAtlas.IsValid() ? t.LocalShadowAtlas : refs.System.White;
            block.Texture( Fill::LocalShadowAtlasTexture, atlas, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( Fill::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Storage( Fill::LocalShadowViewsUB, t.Lights.ShadowViews, RDG::Access::StorageRead );
        }
        fill.Record = [pipeline, dim]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::DispatchCompute( bindings, *pipeline, Common::Math::DivideRoundUp( dim, kGroup ),
                                              Common::Math::DivideRoundUp( dim, kGroup ),
                                              Common::Math::DivideRoundUp( dim * kCascades, kGroup ) );
        };
        nodes.push_back( std::move( fill ) );
        m_FrameVolume = m_Volume;
        return nodes;
    }
} // namespace Desert::Graphic::System

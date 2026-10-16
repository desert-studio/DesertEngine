#include "SkyCapture.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ComputeImages.hpp> // DispatchGroupCount, kComputeImagesWorkGroupSize
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RenderConfig.hpp> // GlobalTextureFilterSampler
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>
#include <bit>
#include <format>

namespace Desert::Graphic::System
{
    namespace
    {
        // SkyCaptureDownsample's and SkyCaptureIrradiance's LocalSize(8, 8, 1); PanoramaToCubemap and
        // PrefilterEnvMap are 32x32 (kComputeImagesWorkGroupSize).
        constexpr uint32_t kSmallGroupSize = 8u;

        struct PrefilterPush
        {
            uint32_t Mip    = 0u;
            uint32_t MaxMip = 0u;
            // PrefilterEnvMap's sample count: 1 = UE's real-time count (32 below roughness 0.1, 64 above), the one
            // a capture that runs on every sky change can pay; the .hdr file's cached bake takes the reference
            // 1024.
            uint32_t RealTime = 1u;
        };

        Image2D* Fallback2D()
        {
            // RGBA32F because the fallback table holds only RGBA8F and RGBA32F (VulkanFallbackTextures.cpp); the
            // format is irrelevant to a branch the shader does not take.
            return FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F ).get();
        }

        const std::shared_ptr<Image2D>& SharedFallback2D()
        {
            return FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F );
        }

        const char* StepName( SkyCaptureStep step )
        {
            switch ( step )
            {
                case SkyCaptureStep::Panorama:
                    return "panorama";
                case SkyCaptureStep::Radiance:
                    return "radiance";
                case SkyCaptureStep::Filter:
                    return "irradiance and prefilter";
                case SkyCaptureStep::Done:
                    return "done";
            }
            return "?";
        }

        SkyCaptureStep NextStep( SkyCaptureStep step )
        {
            return step == SkyCaptureStep::Done ? step
                                                : static_cast<SkyCaptureStep>( static_cast<uint8_t>( step ) + 1u );
        }
    } // namespace

    SkyCapture::~SkyCapture()
    {
        ReleaseCapture();
    }

    bool SkyCapture::MakeProgram( Program& program, const char* shaderName )
    {
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
        if ( !shader )
        {
            LOG_ERROR( "[SkyCapture] the compute shader '{}' is missing, so the sky light cannot be captured.",
                       shaderName );
            return false;
        }
        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = shaderName } );
        if ( !built )
        {
            LOG_ERROR( "[SkyCapture] '{}' has no pipeline, so the sky light cannot be captured: {}", shaderName,
                       built.GetError() );
            return false;
        }
        program.Pipeline = built.GetValue();
        return true;
    }

    Common::BoolResultStr SkyCapture::Initialize()
    {
        // All but the downsample are the .hdr path's too (EnvironmentManager::Create): the panorama resample, the
        // irradiance -- the exact sum over a coarse radiance mip (SkyCaptureIrradiance), one estimator for both
        // -- and the prefilter, at UE's real-time sample count here and the reference count for the cached file.
        // Every program is attempted, so each missing one is named in the log, not only the first.
        bool made = MakeProgram( m_PanoramaToCube, "PanoramaToCubemap" );
        made      = MakeProgram( m_Downsample, "SkyCaptureDownsample" ) && made;
        made      = MakeProgram( m_Irradiance, "SkyCaptureIrradiance" ) && made;
        made      = MakeProgram( m_Prefilter, "PrefilterEnvMap" ) && made;
        if ( !made )
            return Common::MakeError( "the sky capture's compute programs are missing (see the errors above)" );
        return BOOLSUCCESS;
    }

    bool SkyCapture::AcquirePanoramaProgram( const CloudBakeBinding& clouds )
    {
        // THE FOURTH CONSUMER OF THE CLOUD MEDIUM: the panorama that lights the world is compiled under the same
        // substitution the three on-screen cloud programs are, so the light it puts into the world comes from the
        // clouds the camera sees (a second analytic dome beside them is the grey-clouds defect's shape).
        std::shared_ptr<Shader> shader;
        if ( clouds.Medium && !clouds.Medium->IsDefault() )
        {
            shader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "BakeProceduralSky",
                                                                                    *clouds.Medium );
            if ( !shader )
                LOG_ERROR( "[SkyCapture] the authored cloud medium could not be compiled into BakeProceduralSky "
                           "-- the panorama that lights this scene uses the shipped medium, so the world is lit "
                           "by a different sky than the one on screen." );
        }
        if ( !shader )
            shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "BakeProceduralSky" );
        if ( !shader )
        {
            LOG_ERROR( "[SkyCapture] the BakeProceduralSky compute shader is missing; the sky light is not "
                       "captured." );
            return false;
        }
        if ( m_Panorama.Pipeline && m_Panorama.Pipeline->GetSpecification().Shader == shader )
            return true;

        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = "BakeProceduralSky" } );
        if ( !built )
        {
            LOG_ERROR( "[SkyCapture] the procedural sky panorama has no pipeline; the sky light is not captured: "
                       "{}",
                       built.GetError() );
            return false;
        }
        // The program being replaced may still be recorded in a frame in flight; three captures cover them.
        if ( m_Panorama.Pipeline )
        {
            m_RetiredPanorama.push_back( std::move( m_Panorama ) );
            while ( m_RetiredPanorama.size() > 3u )
                m_RetiredPanorama.pop_front();
        }
        m_Panorama          = Program{};
        m_Panorama.Pipeline = built.GetValue();
        return true;
    }

    void SkyCapture::BindPanoramaRoute( const SkyCaptureSource& source )
    {
        ComputePipeline&        pipeline = *m_Panorama.Pipeline;
        const CloudBakeBinding& clouds   = source.Clouds;

        // The sky block the screen pass reads, at the binding number both paths name (kSkyPayloadBinding).
        pipeline.SetStorageBuffer( kSkyPayloadBinding, source.SkyParams );

        // EVERY CLOUD SLOT IS WRITTEN ON EVERY PATH, whether the layer exists or not: a declared descriptor
        // nobody writes invalidates the set, and an invalid set skips the dispatch -- which costs the scene its
        // whole environment, not its clouds, with nothing in the log. The gate is the push constant.
        Image3D* volumeFallback =
             FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get();
        if ( clouds.Params && clouds.Authored )
        {
            pipeline.SetStorageBuffer( kSkyBakeCloudParamsBinding, clouds.Params );
            pipeline.SetStorageBuffer( kSkyBakeCloudAuthoredBinding, clouds.Authored );
        }
        else
        {
            LOG_ERROR( "[SkyCapture] the panorama has no cloud parameter buffer to bind (params={}, authored={}). "
                       "Both are created with the sky's own, so this is an allocation failure -- the dispatch is "
                       "skipped for an invalid descriptor set and the environment comes back black.",
                       clouds.Params != nullptr, clouds.Authored != nullptr );
        }
        const bool cloudsBound = clouds.Marched && clouds.Params != nullptr && clouds.Authored != nullptr;
        const auto sampled     = [&pipeline]( uint32_t binding, Image* image )
        { pipeline.SetInput( binding, image, RDG::Access::SampledCompute, RDG::SubresourceRange::All() ); };

        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
        {
            Image3D* noise = cloudsBound ? clouds.Noise[slot] : nullptr;
            sampled( kSkyBakeCloudNoiseBindings[slot], noise != nullptr ? noise : volumeFallback );
        }
        sampled( kSkyBakeCloudModellingBinding,
                 cloudsBound && clouds.Modelling != nullptr ? clouds.Modelling : volumeFallback );
        sampled( kSkyBakeCloudFarWeatherBinding,
                 cloudsBound && clouds.FarWeather != nullptr ? clouds.FarWeather : Fallback2D() );
        sampled( kSkyBakeCloudLayoutPatternBinding,
                 cloudsBound && clouds.LayoutPattern != nullptr ? clouds.LayoutPattern : Fallback2D() );
        sampled( kSkyBakeCloudLayoutMaskBinding,
                 cloudsBound && clouds.LayoutMask != nullptr ? clouds.LayoutMask : Fallback2D() );
        sampled( kSkyBakeCloudAuthoredAtlasBinding,
                 cloudsBound && clouds.AuthoredAtlas != nullptr ? clouds.AuthoredAtlas : volumeFallback );
        sampled( kSkyBakeCloudSkyOcclusionBinding, cloudsBound && clouds.SkyOcclusion && clouds.SkyOcclusionVolume
                                                        ? clouds.SkyOcclusionVolume
                                                        : volumeFallback );

        // THE AUTHORED MEDIUM'S OWN RESOURCES, bound only when the medium's program declares them (the emitter
        // writes a block only for properties its functions read, and a sampler per Texture2D property).
        if ( clouds.MediumParams )
            pipeline.SetStorageBuffer( Core::kCloudMediumParamsBinding, clouds.MediumParams );
        for ( std::size_t slot = 0; slot < clouds.MediumImages.size(); ++slot )
            sampled( Core::kCloudMediumTextureFirst + static_cast<uint32_t>( slot ),
                     clouds.MediumImages[slot] != nullptr ? clouds.MediumImages[slot] : Fallback2D() );

        // x marches, y reads the sky-occlusion volume, z is the aerial perspective's start depth, w applies the
        // atmosphere's transmittance at each cloud sample's own altitude -- ANDed with the physical model, whose
        // transmittance LUT is the only one that is an atmosphere (the gradient model binds the fallback).
        const bool perSampleSun = cloudsBound && clouds.PerSampleSunTransmittance && source.Physical;
        m_CloudPush             = glm::vec4( cloudsBound ? 1.0f : 0.0f,
                                 cloudsBound && clouds.SkyOcclusion && clouds.SkyOcclusionVolume ? 1.0f : 0.0f,
                                 std::max( clouds.AerialStartDepthKm, 0.0f ), perSampleSun ? 1.0f : 0.0f );
    }

    bool SkyCapture::Begin( const SkyEnvironmentSize& size, SkyCapturePace pace, const SkyCaptureSource& source )
    {
        if ( IsCapturing() )
        {
            LOG_ERROR( "[SkyCapture] a capture was begun while another is at its {} step; the running one "
                       "continues.",
                       StepName( m_Next ) );
            return false;
        }
        if ( !source.SkyParams || !m_PanoramaToCube.Pipeline || !m_Downsample.Pipeline || !m_Irradiance.Pipeline ||
             !m_Prefilter.Pipeline )
        {
            LOG_ERROR( "[SkyCapture] the sky light is not captured: sky block {}, programs {} (see the errors "
                       "above).",
                       source.SkyParams ? "present" : "MISSING",
                       m_PanoramaToCube.Pipeline && m_Prefilter.Pipeline ? "present" : "MISSING" );
            return false;
        }
        if ( !AcquirePanoramaProgram( source.Clouds ) )
            return false;

        // The previous capture's intermediates: no graph built from here on imports them.
        m_RetiredPanoramaImage.reset();
        m_RetiredRadianceImage.reset();

        // A procedural sky's capture products have no file behind them at all.
        const ResourceAttributionScope owned( ResourceOwner::Environment );

        m_PanoramaImage = Image2D::Create(
             Core::Formats::Image2DSpecification{ .Tag        = "ProceduralSkyPanorama",
                                                  .Width      = size.Width,
                                                  .Height     = size.Height,
                                                  .Format     = Core::Formats::ImageFormat::RGBA32F,
                                                  .Mips       = 1u,
                                                  .Usage      = Core::Formats::Image2DUsage::Image2D,
                                                  .Properties = Core::Formats::Storage | Core::Formats::Sample } );
        const auto cube = []( const char* tag, uint32_t face, uint32_t mips )
        {
            return SP_CAST( ImageCube, ImageCube::Create(
                                            Core::Formats::ImageCubeSpecification{
                                                 .Tag        = tag,
                                                 .FaceSize   = face,
                                                 .Format     = Core::Formats::ImageFormat::RGBA32F,
                                                 .Mips       = mips,
                                                 .Properties = Core::Formats::Storage | Core::Formats::Sample },
                                            nullptr ) );
        };
        // The face sizes and mip counts are SkyRules.hpp's (beside the cost report that computes the same
        // numbers): the radiance cube the prefilter convolves, the irradiance, the prefiltered GGX chain.
        m_RadianceImage                = cube( "EnvRadiance", kSkyEnvCubeFaceSize, kSkyEnvRadianceMips );
        std::shared_ptr<ImageCube> irr = cube( "EnvDiffuseIrradiance", kSkyEnvIrradianceFaceSize, 1u );
        std::shared_ptr<ImageCube> pre = cube( "EnvPrefiltered", kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips );
        if ( !m_PanoramaImage || !m_RadianceImage || !irr || !pre )
        {
            LOG_ERROR( "[SkyCapture] the sky light is not captured: an image of the {}x{} capture was not created "
                       "(panorama {}, radiance {}, irradiance {}, prefiltered {}).",
                       size.Width, size.Height, m_PanoramaImage != nullptr, m_RadianceImage != nullptr,
                       irr != nullptr, pre != nullptr );
            m_PanoramaImage.reset();
            m_RadianceImage.reset();
            return false;
        }
        auto* images        = Runtime::ResourceRegistry::GetImageService();
        m_IrradianceHandle  = images->Register( std::move( irr ), Runtime::ImageHandle::Type::ImageCube );
        m_PrefilteredHandle = images->Register( std::move( pre ), Runtime::ImageHandle::Type::ImageCube );

        BindPanoramaRoute( source );
        m_Physical    = source.Physical;
        m_Size        = size;
        m_Pace        = pace;
        m_Next        = SkyCaptureStep::Panorama;
        m_RecordedEnd = SkyCaptureStep::Panorama;
        return true;
    }

    void SkyCapture::ReleaseCapture()
    {
        auto* images = Runtime::ResourceRegistry::GetImageService();
        if ( images != nullptr )
        {
            if ( m_IrradianceHandle.IsValid() )
                images->Unregister( m_IrradianceHandle );
            if ( m_PrefilteredHandle.IsValid() )
                images->Unregister( m_PrefilteredHandle );
        }
        m_IrradianceHandle  = {};
        m_PrefilteredHandle = {};
        m_PanoramaImage.reset();
        m_RadianceImage.reset();
        m_Next        = SkyCaptureStep::Done;
        m_RecordedEnd = SkyCaptureStep::Done;
    }

    bool SkyCapture::AddPasses( RDG::Builder& graph, FrameTextures& textures, const SkyCaptureFrameReads& reads )
    {
        m_RecordedEnd = m_Next;
        if ( !IsCapturing() )
            return true;

        // Every image this slice names is imported first, so a refusal adds no pass at all.
        auto* images = Runtime::ResourceRegistry::GetImageService();
        if ( !textures.Import( m_PanoramaImage, "SkyCapture.Panorama" ).IsValid() ||
             !textures.Import( m_RadianceImage, "SkyCapture.Radiance" ).IsValid() ||
             !textures.Import( images->Share( m_IrradianceHandle ), "SkyCapture.Irradiance" ).IsValid() ||
             !textures.Import( images->Share( m_PrefilteredHandle ), "SkyCapture.Prefiltered" ).IsValid() )
            return false;

        const SkyCaptureStep end = m_Pace == SkyCapturePace::Whole ? SkyCaptureStep::Done : NextStep( m_Next );
        for ( SkyCaptureStep step = m_Next; step != end; step = NextStep( step ) )
        {
            switch ( step )
            {
                case SkyCaptureStep::Panorama:
                    AddPanorama( graph, textures, reads );
                    break;
                case SkyCaptureStep::Radiance:
                    AddRadiance( graph, textures );
                    break;
                case SkyCaptureStep::Filter:
                    AddIrradiance( graph, textures );
                    AddPrefilter( graph, textures );
                    break;
                case SkyCaptureStep::Done:
                    break;
            }
        }
        m_RecordedEnd = end;
        return true;
    }

    std::optional<Environment> SkyCapture::Settle( bool accepted )
    {
        if ( !IsCapturing() )
            return std::nullopt;
        if ( !accepted )
        {
            LOG_ERROR( "[SkyCapture] the frame graph refused the {}x{} sky capture's {} step; the capture is "
                       "abandoned and the environment the scene reads is kept.",
                       m_Size.Width, m_Size.Height, StepName( m_Next ) );
            ReleaseCapture();
            return std::nullopt;
        }
        m_Next = m_RecordedEnd;
        if ( m_Next != SkyCaptureStep::Done )
            return std::nullopt;

        // COMPLETE: the two cubes go to the caller; the intermediates stay until the next capture begins, because
        // this frame's graph has imported them and is still being built.
        Environment captured{ Common::Filepath( "ProceduralSky" ), Runtime::ImageHandle{}, m_IrradianceHandle,
                              m_PrefilteredHandle };
        m_IrradianceHandle     = {};
        m_PrefilteredHandle    = {};
        m_RetiredPanoramaImage = std::move( m_PanoramaImage );
        m_RetiredRadianceImage = std::move( m_RadianceImage );
        return captured;
    }

    void SkyCapture::AddPanorama( RDG::Builder& graph, FrameTextures& textures, const SkyCaptureFrameReads& reads )
    {
        // The LUTs the physical march samples, and the distant sky light the cloud ambient reads: this frame's
        // graph writes them before this pass (SkyboxRenderer::DeclareAtmosphereCachedLutNodes), so they are
        // declared reads, ordered and barriered by the graph. The gradient model binds the fallback.
        const std::shared_ptr<Image2D>& transmittance =
             m_Physical && reads.TransmittanceLut ? reads.TransmittanceLut : SharedFallback2D();
        const std::shared_ptr<Image2D>& multiScatter =
             m_Physical && reads.MultiScatterLut ? reads.MultiScatterLut : SharedFallback2D();
        const std::shared_ptr<Image2D>& distant =
             reads.DistantSkyLight ? reads.DistantSkyLight : SharedFallback2D();

        const RDG::TextureRef  panorama   = textures.Import( m_PanoramaImage, "SkyCapture.Panorama" );
        const RDG::TextureRef  lutT       = textures.Import( transmittance, "Sky.TransmittanceLut" );
        const RDG::TextureRef  lutM       = textures.Import( multiScatter, "Sky.MultiScatterLut" );
        const RDG::TextureRef  distantSky = textures.Import( distant, "Sky.DistantLight" );
        const uint32_t         width      = m_Size.Width;
        const uint32_t         height     = m_Size.Height;
        Program&               program    = m_Panorama;
        const glm::vec4        push       = m_CloudPush;
        const RDG::SamplerDesc ownSampler = GlobalTextureFilterSampler();

        graph.AddPass(
             "Sky Capture: Panorama", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( program.Layout.Get( program.Pipeline->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *program.Pipeline ) )
                      .Sampled( "u_TransmittanceLut", lutT, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), ownSampler )
                      .Sampled( "u_MultiScatterLut", lutM, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), ownSampler )
                      .Sampled( "u_DistantSkyLight", distantSky, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), ownSampler )
                      .Storage( "outputPanorama", panorama, RDG::Access::StorageWrite, 0 )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::vec4 ) ) );
             },
             [pipeline = program.Pipeline, push, width,
              height]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "Sky Capture: Panorama" );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *pipeline, std::max( 1u, width / kComputeImagesWorkGroupSize ),
                      std::max( 1u, height / kComputeImagesWorkGroupSize ), 1u );
             } );
    }

    void SkyCapture::AddRadiance( RDG::Builder& graph, FrameTextures& textures )
    {
        const RDG::TextureRef  panorama   = textures.Import( m_PanoramaImage, "SkyCapture.Panorama" );
        const RDG::TextureRef  radiance   = textures.Import( m_RadianceImage, "SkyCapture.Radiance" );
        const RDG::SamplerDesc ownSampler = GlobalTextureFilterSampler();

        graph.AddPass(
             "Sky Capture: Radiance", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings(
                          m_PanoramaToCube.Layout.Get( m_PanoramaToCube.Pipeline->GetSpecification().Shader ),
                          Renderer::GetPipelineRouteFill( *m_PanoramaToCube.Pipeline ) )
                      .Sampled( "inputTexture", panorama, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), ownSampler )
                      .Storage( "outputTexture", radiance, RDG::Access::StorageWrite, 0 );
             },
             [pipeline = m_PanoramaToCube.Pipeline]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "Sky Capture: Radiance" );
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 const uint32_t groups = DispatchGroupCount( kSkyEnvCubeFaceSize, kComputeImagesWorkGroupSize );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups, groups, 6u );
             } );

        // The radiance cube's mip chain, each mip the 2x2 box of the one below it (the blit chain the GpuBatch
        // bake recorded, as one compute dispatch per mip): the GGX prefilter's filtered importance sampling reads
        // them.
        for ( uint32_t mip = 1; mip < kSkyEnvRadianceMips; ++mip )
            graph.AddPass(
                 std::format( "Sky Capture: Radiance mip {}", mip ),
                 RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Downsample.Layout.Get( m_Downsample.Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Downsample.Pipeline ) )
                          .Sampled( "u_Source", radiance, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( mip - 1u ), RDG::SamplerDesc::LinearClamp() )
                          .Storage( "u_Output", radiance, RDG::Access::StorageWrite, mip );
                 },
                 [pipeline = m_Downsample.Pipeline, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Sky Capture: Radiance mip" );
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     const uint32_t          groups =
                          DispatchGroupCount( std::max( 1u, kSkyEnvCubeFaceSize >> mip ), kSmallGroupSize );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups, groups, 6u );
                 } );
    }

    void SkyCapture::AddIrradiance( RDG::Builder& graph, FrameTextures& textures )
    {
        auto*                 images   = Runtime::ResourceRegistry::GetImageService();
        const RDG::TextureRef radiance = textures.Import( m_RadianceImage, "SkyCapture.Radiance" );
        const RDG::TextureRef irradiance =
             textures.Import( images->Share( m_IrradianceHandle ), "SkyCapture.Irradiance" );

        graph.AddPass(
             "Sky Capture: Irradiance", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_Irradiance.Layout.Get( m_Irradiance.Pipeline->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_Irradiance.Pipeline ) )
                      .Sampled( "u_Radiance", radiance, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::Mip( kSkyEnvIrradianceSourceMip ),
                                RDG::SamplerDesc::PointClamp() )
                      .Storage( "u_Output", irradiance, RDG::Access::StorageWrite, 0 );
             },
             [pipeline = m_Irradiance.Pipeline]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "Sky Capture: Irradiance" );
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 const uint32_t          groups = DispatchGroupCount( kSkyEnvIrradianceFaceSize, kSmallGroupSize );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups, groups, 6u );
             } );
    }

    void SkyCapture::AddPrefilter( RDG::Builder& graph, FrameTextures& textures )
    {
        auto*                 images   = Runtime::ResourceRegistry::GetImageService();
        const RDG::TextureRef radiance = textures.Import( m_RadianceImage, "SkyCapture.Radiance" );
        const RDG::TextureRef prefiltered =
             textures.Import( images->Share( m_PrefilteredHandle ), "SkyCapture.Prefiltered" );
        const RDG::SamplerDesc ownSampler = GlobalTextureFilterSampler();

        // One dispatch per mip, each convolved with the matching GGX roughness: the shader derives it from the mip
        // (Common/ReflectionCaptureMip.glslh, the engine's one ramp, which the lighting reads back).
        for ( uint32_t mip = 0; mip < kSkyEnvPrefilterMips; ++mip )
            graph.AddPass(
                 std::format( "Sky Capture: Prefilter mip {}", mip ),
                 RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Prefilter.Layout.Get( m_Prefilter.Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Prefilter.Pipeline ) )
                          .Sampled( "inputTexture", radiance, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), ownSampler )
                          .Storage( "outputTexture", prefiltered, RDG::Access::StorageWrite, mip )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( PrefilterPush ) ) );
                 },
                 [pipeline = m_Prefilter.Pipeline, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Sky Capture: Prefilter" );
                     const PrefilterPush push{ mip, kSkyEnvPrefilterMips - 1u, 1u };
                     RDG::PassBindings   bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     const uint32_t groups = DispatchGroupCount( std::max( 1u, kSkyEnvPrefilterFaceSize >> mip ),
                                                                 kComputeImagesWorkGroupSize );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups, groups, 6u );
                 } );
    }
} // namespace Desert::Graphic::System

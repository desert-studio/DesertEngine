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
#include <string>

namespace Desert::Graphic::System
{
    namespace
    {
        // SkyCaptureDownsample's, SkyCaptureIrradiance's and BakeProceduralSky's LocalSize(8, 8, 1);
        // PrefilterEnvMap is 32x32 (kComputeImagesWorkGroupSize).
        constexpr uint32_t kSmallGroupSize = 8u;

        // BakeProceduralSky's push block: the cloud gate (SkyCapture::m_CloudPush) and the rows this dispatch
        // renders -- its z counts faces from FirstFace, its y rows from FirstRow.
        struct FacesPush
        {
            glm::vec4 Clouds{ 0.0f };
            uint32_t  FirstFace = 0u;
            uint32_t  FirstRow  = 0u;
            uint32_t  Pad[2]    = { 0u, 0u };
        };

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
                case SkyCaptureStep::Faces:
                    return "faces";
                case SkyCaptureStep::Filter:
                    return "mips, irradiance and prefilter";
                case SkyCaptureStep::Done:
                    return "done";
            }
            return "?";
        }
    } // namespace

    std::vector<SkyCaptureFaceSlice> SkyCaptureFaceSlices( uint32_t face )
    {
        // A band is never thinner than one 8-row group.
        const uint32_t bandRows = std::max( face / kSkyCaptureBandsPerFace, std::min( face, kSmallGroupSize ) );
        std::vector<SkyCaptureFaceSlice> slices;
        for ( uint32_t index = 0; index < kSkyCaptureFaceCount; ++index )
        {
            // The rows that can see the sky: all of the up face, the upper half of a side face, none of the down.
            const uint32_t skyRows = index == 2u ? face : index == 3u ? 0u : face / 2u;
            for ( uint32_t row = 0; row < skyRows; row += bandRows )
                slices.push_back(
                     { .Face = index, .FirstRow = row, .Rows = std::min( bandRows, skyRows - row ) } );
            if ( skyRows < face )
                slices.push_back( { .Face = index, .FirstRow = skyRows, .Rows = face - skyRows } );
        }
        return slices;
    }

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
        // The irradiance and the prefilter are the .hdr path's too (EnvironmentManager::Create): the exact sum
        // over the 16-texel radiance mip (SkyCaptureIrradiance), one estimator for both, and the prefilter, at
        // UE's real-time sample count here and the reference count for the cached file. Every program is
        // attempted, so each missing one is named in the log, not only the first.
        bool made = MakeProgram( m_Downsample, "SkyCaptureDownsample" );
        made      = MakeProgram( m_Irradiance, "SkyCaptureIrradiance" ) && made;
        made      = MakeProgram( m_Prefilter, "PrefilterEnvMap" ) && made;
        if ( !made )
            return Common::MakeError( "the sky capture's compute programs are missing (see the errors above)" );
        return BOOLSUCCESS;
    }

    bool SkyCapture::AcquireFacesProgram( const CloudBakeBinding& clouds )
    {
        // THE FOURTH CONSUMER OF THE CLOUD MEDIUM: the faces that light the world is compiled under the same
        // substitution the three on-screen cloud programs are, so the light it puts into the world comes from the
        // clouds the camera sees (a second analytic dome beside them is the grey-clouds defect's shape).
        std::shared_ptr<Shader> shader;
        if ( clouds.Medium && !clouds.Medium->IsDefault() )
        {
            shader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "BakeProceduralSky",
                                                                                    *clouds.Medium );
            if ( !shader )
                LOG_ERROR( "[SkyCapture] the authored cloud medium could not be compiled into BakeProceduralSky "
                           "-- the sky light of this scene uses the shipped medium, so the world is lit "
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
        if ( m_Faces.Pipeline && m_Faces.Pipeline->GetSpecification().Shader == shader )
            return true;

        const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = "BakeProceduralSky" } );
        if ( !built )
        {
            LOG_ERROR( "[SkyCapture] the procedural sky's capture faces have no pipeline; the sky light is not "
                       "captured: "
                       "{}",
                       built.GetError() );
            return false;
        }
        // The program being replaced may still be recorded in a frame in flight; three captures cover them.
        if ( m_Faces.Pipeline )
        {
            m_RetiredFaces.push_back( std::move( m_Faces ) );
            while ( m_RetiredFaces.size() > 3u )
                m_RetiredFaces.pop_front();
        }
        m_Faces          = Program{};
        m_Faces.Pipeline = built.GetValue();
        return true;
    }

    void SkyCapture::BindFacesRoute( const SkyCaptureSource& source )
    {
        ComputePipeline&        pipeline = *m_Faces.Pipeline;
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
            pipeline.SetStorageBuffer( kSkyBakeCloudParamsBinding, clouds.Params.get() );
            pipeline.SetStorageBuffer( kSkyBakeCloudAuthoredBinding, clouds.Authored.get() );
        }
        else
        {
            LOG_ERROR(
                 "[SkyCapture] the capture faces have no cloud parameter buffer to bind (params={}, authored={}). "
                 "Both are created with the sky's own, so this is an allocation failure -- the dispatch is "
                 "skipped for an invalid descriptor set and the environment comes back black.",
                 clouds.Params != nullptr, clouds.Authored != nullptr );
        }
        const bool cloudsBound = clouds.Marched && clouds.Params != nullptr && clouds.Authored != nullptr;
        const auto sampled     = [&pipeline]( uint32_t binding, Image* image )
        { pipeline.SetInput( binding, image, RDG::Access::SampledCompute, RDG::SubresourceRange::All() ); };

        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
        {
            Image3D* noise = cloudsBound ? clouds.Noise[slot].get() : nullptr;
            sampled( kSkyBakeCloudNoiseBindings[slot], noise != nullptr ? noise : volumeFallback );
        }
        sampled( kSkyBakeCloudModellingBinding,
                 cloudsBound && clouds.Modelling != nullptr ? clouds.Modelling.get() : volumeFallback );
        sampled( kSkyBakeCloudFarWeatherBinding,
                 cloudsBound && clouds.FarWeather != nullptr ? clouds.FarWeather.get() : Fallback2D() );
        sampled( kSkyBakeCloudLayoutPatternBinding,
                 cloudsBound && clouds.LayoutPattern != nullptr ? clouds.LayoutPattern.get() : Fallback2D() );
        sampled( kSkyBakeCloudLayoutMaskBinding,
                 cloudsBound && clouds.LayoutMask != nullptr ? clouds.LayoutMask.get() : Fallback2D() );
        sampled( kSkyBakeCloudAuthoredAtlasBinding,
                 cloudsBound && clouds.AuthoredAtlas != nullptr ? clouds.AuthoredAtlas.get() : volumeFallback );
        sampled( kSkyBakeCloudSkyOcclusionBinding, cloudsBound && clouds.SkyOcclusion && clouds.SkyOcclusionVolume
                                                        ? clouds.SkyOcclusionVolume.get()
                                                        : volumeFallback );

        // THE AUTHORED MEDIUM'S OWN RESOURCES, bound only when the medium's program declares them (the emitter
        // writes a block only for properties its functions read, and a sampler per Texture2D property).
        if ( clouds.MediumParams )
            pipeline.SetStorageBuffer( Core::kCloudMediumParamsBinding, clouds.MediumParams.get() );
        for ( std::size_t slot = 0; slot < clouds.MediumImages.size(); ++slot )
            sampled( Core::kCloudMediumTextureFirst + static_cast<uint32_t>( slot ),
                     clouds.MediumImages[slot] != nullptr ? clouds.MediumImages[slot].get() : Fallback2D() );

        // Every texture above is a separate one read through a shared state (MAT-BINDLESS): the cloud volumes and
        // maps through trilinear REPEAT, the state the screen march reads them with; the medium's own textures
        // through the state right after them (ShaderGraph's Volume-domain emitter), as VolumetricCloudRenderer's
        // u_CloudMediumSampler. The LUTs' state (kSkyBakeGlobalFilterSamplerBinding) is the graph block's.
        pipeline.SetSampler( kSkyBakeLinearRepeatSamplerBinding, RDG::SamplerDesc::LinearRepeat() );
        if ( !clouds.MediumImages.empty() )
            pipeline.SetSampler( Core::kCloudMediumTextureFirst + static_cast<uint32_t>( clouds.MediumImages.size() ),
                                 GlobalTextureFilterSampler() );

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
        if ( !source.SkyParams || !m_Downsample.Pipeline || !m_Irradiance.Pipeline || !m_Prefilter.Pipeline )
        {
            LOG_ERROR( "[SkyCapture] the sky light is not captured: sky block {}, programs {} (see the errors "
                       "above).",
                       source.SkyParams ? "present" : "MISSING",
                       m_Downsample.Pipeline && m_Irradiance.Pipeline && m_Prefilter.Pipeline ? "present"
                                                                                              : "MISSING" );
            return false;
        }
        if ( !AcquireFacesProgram( source.Clouds ) )
            return false;
        const uint32_t face = size.Face;
        if ( !std::has_single_bit( face ) || face < kSkyEnvIrradianceSourceFace )
        {
            LOG_ERROR( "[SkyCapture] the sky light is not captured: a {} face has no {}-texel mip to sum the "
                       "irradiance over.",
                       face, kSkyEnvIrradianceSourceFace );
            return false;
        }

        if ( !AllocateCubes( face, face, false ) )
            return false;
        m_Source            = Common::Filepath( "ProceduralSky" );
        m_PrefilterRealTime = 1u;

        BindFacesRoute( source );
        // THE CAPTURE CO-OWNS WHAT ITS ROUTE NAMES until its last slice is recorded: the route holds raw
        // descriptors, read by every face slice over the frames after this one, and the cloud renderer may
        // reallocate a volume (or the beginning view go away with its cloud blocks) meanwhile. The medium
        // pointer is Begin's argument only -- its program is already acquired.
        m_Clouds            = source.Clouds;
        m_Clouds.Medium     = nullptr;
        m_Physical          = source.Physical;
        m_Size              = size;
        m_Pace              = pace;
        m_Next              = SkyCaptureStep::Faces;
        m_RecordedEnd       = SkyCaptureStep::Faces;
        m_FaceSlices        = SkyCaptureFaceSlices( face );
        m_FaceSlice         = 0u;
        m_RecordedFaceSlice = 0u;
        return true;
    }

    bool SkyCapture::AllocateCubes( uint32_t radianceFace, uint32_t prefilterFace, bool keepRadiance )
    {
        // The previous capture's intermediate: no graph built from here on imports it.
        m_RetiredRadianceImage.reset();

        // A sky light's products are the environment's, not an asset's: the recipe that rebuilds them is a capture.
        const ResourceAttributionScope owned( ResourceOwner::Environment );

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
        // The faces and mip counts are SkyRules.hpp's (beside the cost report that computes the same numbers): the
        // radiance cube the faces are rendered into and the prefilter convolves, the irradiance, and the
        // prefiltered GGX chain (a captured sky's at its own face, as UE filters its real-time capture; a
        // specified cubemap's at the cached file's). The lighting reads the chain's length from the cube.
        std::shared_ptr<ImageCube> radiance =
             cube( "EnvRadiance", radianceFace, Core::Formats::MipChainLength( radianceFace ) );
        std::shared_ptr<ImageCube> irr = cube( "EnvDiffuseIrradiance", kSkyEnvIrradianceFaceSize, 1u );
        std::shared_ptr<ImageCube> pre =
             cube( "EnvPrefiltered", prefilterFace, Core::Formats::MipChainLength( prefilterFace ) );
        if ( !radiance || !irr || !pre )
        {
            LOG_ERROR( "[SkyCapture] the sky light is not captured: an image of the {} face capture was not "
                       "created (radiance {}, irradiance {}, prefiltered {}).",
                       radianceFace, radiance != nullptr, irr != nullptr, pre != nullptr );
            return false;
        }
        auto* images = Runtime::ResourceRegistry::GetImageService();
        if ( keepRadiance )
        {
            m_RadianceHandle = images->Register( std::move( radiance ), Runtime::ImageHandle::Type::ImageCube );
            radiance         = SP_CAST( ImageCube, images->Share( m_RadianceHandle ) );
        }
        m_RadianceImage     = std::move( radiance );
        m_IrradianceHandle  = images->Register( std::move( irr ), Runtime::ImageHandle::Type::ImageCube );
        m_PrefilteredHandle = images->Register( std::move( pre ), Runtime::ImageHandle::Type::ImageCube );
        m_PrefilterFace     = prefilterFace;
        return true;
    }

    bool SkyCapture::BeginPanorama( const SkyCapturePanorama& source )
    {
        if ( IsCapturing() )
        {
            LOG_ERROR( "[SkyCapture] the specified cubemap '{}' was begun while a capture is at its {} step; the "
                       "running one continues.",
                       source.Source.string(), StepName( m_Next ) );
            return false;
        }
        if ( !m_PanoramaToCube.Pipeline && !MakeProgram( m_PanoramaToCube, "PanoramaToCubemap" ) )
            return false;
        if ( !source.Panorama || !m_Downsample.Pipeline || !m_Irradiance.Pipeline || !m_Prefilter.Pipeline )
        {
            LOG_ERROR( "[SkyCapture] the specified cubemap '{}' is not converted: panorama {}, programs {} (see the "
                       "errors above).",
                       source.Source.string(), source.Panorama ? "present" : "MISSING",
                       m_Downsample.Pipeline && m_Irradiance.Pipeline && m_Prefilter.Pipeline ? "present"
                                                                                              : "MISSING" );
            return false;
        }
        if ( !AllocateCubes( kSkyEnvCubeFaceSize, kSkyEnvPrefilterFaceSize, true ) )
            return false;
        m_Panorama          = source.Panorama;
        m_Source            = source.Source;
        m_PrefilterRealTime = 0u;
        m_Clouds            = {};
        m_Physical          = false;
        m_Size              = SkyEnvironmentSize{ kSkyEnvCubeFaceSize };
        m_Pace              = SkyCapturePace::Whole;
        m_Next              = SkyCaptureStep::Faces;
        m_RecordedEnd       = SkyCaptureStep::Faces;
        m_FaceSlices.clear();
        m_FaceSlice         = 0u;
        m_RecordedFaceSlice = 0u;
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
            if ( m_RadianceHandle.IsValid() )
                images->Unregister( m_RadianceHandle );
        }
        m_IrradianceHandle  = {};
        m_PrefilteredHandle = {};
        m_RadianceHandle    = {};
        m_RadianceImage.reset();
        m_Panorama.reset();
        m_Clouds      = {};
        m_Next        = SkyCaptureStep::Done;
        m_RecordedEnd = SkyCaptureStep::Done;
    }

    bool SkyCapture::AddPasses( RDG::Builder& graph, FrameTextures& textures, const SkyCaptureFrameReads& reads )
    {
        m_RecordedEnd       = m_Next;
        m_RecordedFaceSlice = m_FaceSlice;
        if ( !IsCapturing() )
            return true;

        // Every image this slice names is imported first, so a refusal adds no pass at all.
        auto* images = Runtime::ResourceRegistry::GetImageService();
        if ( !textures.Import( m_RadianceImage, "SkyCapture.Radiance" ).IsValid() ||
             !textures.Import( images->Share( m_IrradianceHandle ), "SkyCapture.Irradiance" ).IsValid() ||
             !textures.Import( images->Share( m_PrefilteredHandle ), "SkyCapture.Prefiltered" ).IsValid() )
            return false;

        // ONE LAYOUT AT THE END OF EVERY SLICE. A face slice writes one face of mip 0 and leaves the other mips as
        // they were, and an image whose subresources end a graph in different layouts cannot be recorded for the
        // next graph's import (VulkanRenderer's end-of-graph check refuses it): the whole cube ends sampled.
        if ( const auto extracted =
                  textures.ExtractImported( m_RadianceImage, "SkyCapture.Radiance", RDG::Access::SampledCompute );
             !extracted.IsSuccess() )
        {
            LOG_ERROR( "[SkyCapture] the radiance cube's end state was not declared: {}", extracted.GetError() );
            return false;
        }

        // WHOLE: every remaining step, the six faces in one dispatch. TIME-SLICED: one step, and of the faces step
        // one SkyCaptureFaceSlice -- it stays the next step until its last slice is recorded.
        const uint32_t sliceCount = static_cast<uint32_t>( m_FaceSlices.size() );
        SkyCaptureStep step       = m_Next;
        uint32_t       faceSlice  = m_FaceSlice;
        do
        {
            switch ( step )
            {
                case SkyCaptureStep::Faces:
                {
                    if ( m_Panorama )
                    {
                        AddPanoramaFaces( graph, textures );
                        faceSlice = sliceCount;
                    }
                    else if ( m_Pace == SkyCapturePace::Whole && faceSlice == 0u )
                    {
                        AddFaces( graph, textures, reads, 0u, kSkyCaptureFaceCount, 0u, m_Size.Face );
                        faceSlice = sliceCount;
                    }
                    else if ( faceSlice < sliceCount )
                    {
                        const SkyCaptureFaceSlice& slice = m_FaceSlices[faceSlice];
                        AddFaces( graph, textures, reads, slice.Face, 1u, slice.FirstRow, slice.Rows );
                        ++faceSlice;
                    }
                    if ( faceSlice >= sliceCount )
                        step = SkyCaptureStep::Filter;
                    break;
                }
                case SkyCaptureStep::Filter:
                    AddRadianceMips( graph, textures );
                    AddIrradiance( graph, textures );
                    AddPrefilter( graph, textures );
                    step = SkyCaptureStep::Done;
                    break;
                case SkyCaptureStep::Done:
                    break;
            }
        } while ( m_Pace == SkyCapturePace::Whole && step != SkyCaptureStep::Done );
        m_RecordedEnd       = step;
        m_RecordedFaceSlice = faceSlice;
        return true;
    }

    std::optional<Environment> SkyCapture::Settle( bool accepted )
    {
        if ( !IsCapturing() )
            return std::nullopt;
        if ( !accepted )
        {
            LOG_ERROR( "[SkyCapture] the frame graph refused the {} face sky capture's {} step (slice {}); the "
                       "capture is abandoned and the environment the scene reads is kept.",
                       m_Size.Face, StepName( m_Next ), m_FaceSlice );
            ReleaseCapture();
            return std::nullopt;
        }
        m_Next = m_RecordedEnd;
        m_FaceSlice = m_RecordedFaceSlice;
        if ( m_Next != SkyCaptureStep::Done )
            return std::nullopt;

        // COMPLETE: the cubes go to the caller -- a captured sky's two, a specified cubemap's radiance cube as well
        // (the backdrop draws it); the intermediate reference stays until the next capture begins, because this
        // frame's graph has imported it and is still being built.
        Environment captured{ m_Source, m_RadianceHandle, m_IrradianceHandle, m_PrefilteredHandle };
        m_IrradianceHandle     = {};
        m_PrefilteredHandle    = {};
        m_RadianceHandle       = {};
        m_Panorama.reset();
        m_RetiredRadianceImage = std::move( m_RadianceImage );
        // The cloud volumes and blocks the faces read: this frame's graph recorded the last face slice already,
        // and an image or buffer released here is destroyed past the frames in flight.
        m_Clouds = {};
        return captured;
    }

    void SkyCapture::AddFaces( RDG::Builder& graph, FrameTextures& textures, const SkyCaptureFrameReads& reads,
                               uint32_t firstFace, uint32_t faceCount, uint32_t firstRow, uint32_t rowCount )
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

        if ( reads.SkyParams )
            m_Faces.Pipeline->SetStorageBuffer( kSkyPayloadBinding, reads.SkyParams );

        const RDG::TextureRef  radiance   = textures.Import( m_RadianceImage, "SkyCapture.Radiance" );
        const RDG::TextureRef  lutT       = textures.Import( transmittance, "Sky.TransmittanceLut" );
        const RDG::TextureRef  lutM       = textures.Import( multiScatter, "Sky.MultiScatterLut" );
        const RDG::TextureRef  distantSky = textures.Import( distant, "Sky.DistantLight" );
        const uint32_t         face       = m_Size.Face;
        Program&               program    = m_Faces;
        const FacesPush        push{ .Clouds = m_CloudPush, .FirstFace = firstFace, .FirstRow = firstRow };
        const RDG::SamplerDesc ownSampler = GlobalTextureFilterSampler();

        graph.AddPass(
             faceCount == kSkyCaptureFaceCount
                  ? std::string( "Sky Capture: Faces" )
                  : std::format( "Sky Capture: Face {} rows {}", firstFace, firstRow ),
             RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( program.Layout.Get( program.Pipeline->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *program.Pipeline ) )
                      .Texture( "u_TransmittanceLut", lutT, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Texture( "u_MultiScatterLut", lutM, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Texture( "u_DistantSkyLight", distantSky, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Sampler( "u_GlobalFilterSampler", ownSampler )
                      .Storage( "outputFaces", radiance, RDG::Access::StorageWrite, 0 )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( FacesPush ) ) );
             },
             [pipeline = program.Pipeline, push, face, faceCount,
              rowCount]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "Sky Capture: Faces" );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *pipeline, DispatchGroupCount( face, kSmallGroupSize ),
                      DispatchGroupCount( rowCount, kSmallGroupSize ), faceCount );
             } );
    }

    void SkyCapture::AddPanoramaFaces( RDG::Builder& graph, FrameTextures& textures )
    {
        const RDG::TextureRef radiance = textures.Import( m_RadianceImage, "SkyCapture.Radiance" );
        const RDG::TextureRef panorama = textures.Import( m_Panorama, "SkyCapture.Panorama" );
        const uint32_t        face     = m_Size.Face;

        graph.AddPass(
             "Sky Capture: Panorama to cube", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_PanoramaToCube.Layout.Get( m_PanoramaToCube.Pipeline->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_PanoramaToCube.Pipeline ) )
                      .Texture( "inputTexture", panorama, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
                      .Storage( "outputTexture", radiance, RDG::Access::StorageWrite, 0 );
             },
             [pipeline = m_PanoramaToCube.Pipeline, face]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "Sky Capture: Panorama to cube" );
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 const uint32_t          groups = DispatchGroupCount( face, kComputeImagesWorkGroupSize );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups, groups, 6u );
             } );
    }

    void SkyCapture::AddRadianceMips( RDG::Builder& graph, FrameTextures& textures )
    {
        const RDG::TextureRef radiance = textures.Import( m_RadianceImage, "SkyCapture.Radiance" );
        const uint32_t        face     = m_Size.Face;
        const uint32_t        mips     = Core::Formats::MipChainLength( face );

        // The radiance cube's mip chain, each mip the 2x2 box of the one below it: the GGX prefilter's filtered
        // importance sampling reads them, and the irradiance sums its 16-texel mip.
        for ( uint32_t mip = 1; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "Sky Capture: Radiance mip {}", mip ),
                 RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Downsample.Layout.Get( m_Downsample.Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Downsample.Pipeline ) )
                          .Texture( "u_Source", radiance, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( mip - 1u ) )
                          .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                          .Storage( "u_Output", radiance, RDG::Access::StorageWrite, mip );
                 },
                 [pipeline = m_Downsample.Pipeline, mip,
                  face]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Sky Capture: Radiance mip" );
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     const uint32_t groups = DispatchGroupCount( std::max( 1u, face >> mip ), kSmallGroupSize );
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
                      .Texture( "u_Radiance", radiance, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::Mip( SkyCaptureIrradianceSourceMip( m_Size.Face ) ) )
                      .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
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
        // A specified cubemap's chain is written to its cache file, and a display setting must not change what is
        // cached: it reads through trilinear REPEAT (kComputeImagesSamplerBinding's state), a captured sky through
        // the global texture filter as every other view-time read.
        const RDG::SamplerDesc ownSampler = m_Panorama ? RDG::SamplerDesc::LinearRepeat() : GlobalTextureFilterSampler();

        // One dispatch per mip, each convolved with the matching GGX roughness: the shader derives it from the mip
        // (Common/ReflectionCaptureMip.glslh, the engine's one ramp, which the lighting reads back).
        const uint32_t face     = m_PrefilterFace;
        const uint32_t mips     = Core::Formats::MipChainLength( face );
        const uint32_t realTime = m_PrefilterRealTime;
        for ( uint32_t mip = 0; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "Sky Capture: Prefilter mip {}", mip ),
                 RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Prefilter.Layout.Get( m_Prefilter.Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Prefilter.Pipeline ) )
                          .Texture( "inputTexture", radiance, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_LinearRepeatSampler", ownSampler )
                          .Storage( "outputTexture", prefiltered, RDG::Access::StorageWrite, mip )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( PrefilterPush ) ) );
                 },
                 [pipeline = m_Prefilter.Pipeline, mip, face, mips,
                  realTime]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Sky Capture: Prefilter" );
                     const PrefilterPush push{ mip, mips - 1u, realTime };
                     RDG::PassBindings   bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     const uint32_t groups =
                          DispatchGroupCount( std::max( 1u, face >> mip ), kComputeImagesWorkGroupSize );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups, groups, 6u );
                 } );
    }
} // namespace Desert::Graphic::System

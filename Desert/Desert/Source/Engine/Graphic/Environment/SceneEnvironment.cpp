#include <Engine/Graphic/Environment/SceneEnvironment.hpp>

#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ComputeImages.hpp>
#include <Engine/Graphic/SkyRules.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>

namespace Desert::Graphic
{
    Environment EnvironmentManager::Create( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset,
                                            const SkyLook&                              look )
    {
        // The panorama, the three cubes and the transient compute pipelines the bake creates are all the
        // ENVIRONMENT's, not the skybox asset's: the recipe that rebuilds them is the sky settings plus a
        // 450 ms bake, not a file read, so eviction must never treat them as reloadable. One scope rather
        // than five claims — see Engine/Graphic/ResourceLedger.hpp on ResourceAttributionScope.
        const ResourceAttributionScope owned( ResourceOwner::Environment );

        if ( Common::Utils::FileSystem::GetFileExtension( skyboxAsset->GetMetadata().Filepath ) ==
             ".hdr" ) // TODO: move the logic to the SkyboxAsset and raw data
        {
            // The asset's metadata carries the FULL path (registration owns path composition) — the
            // engine draw layer never glues directory prefixes onto asset paths.
            // A PANORAMA THAT DID NOT LOAD USED TO BE DEREFERENCED ON THE NEXT LINE. `ExtractValue()`
            // on a failed Create handed back a null shared_ptr in silence, and
            // `imagePanorama->GetImageHandle()` below is an unconditional dereference — so a skybox
            // whose .hdr was missing, unreadable or malformed took the process down rather than
            // leaving the scene without an environment. An empty Environment is a shape this function
            // already produces (the non-.hdr return below), so the caller needs nothing new.
            auto panorama = Texture2D::Create( { true }, skyboxAsset->GetMetadata().Filepath );
            if ( !panorama )
            {
                LOG_ERROR( "[SceneEnvironment] the skybox panorama '{}' did not load, so this scene gets "
                           "NO environment (no radiance, no irradiance, no prefiltered specular): {}",
                           skyboxAsset->GetMetadata().Filepath.string(), panorama.GetError() );
                return {};
            }
            std::shared_ptr<Texture2D> imagePanorama = panorama.ExtractValue();

            auto* imageService = Runtime::ResourceRegistry::GetImageService();

            // 1) Radiance cube (sharp environment) — also the source the prefilter convolves.
            auto        radianceCube   = ConvertPanoramaToRadianceCube( imagePanorama->GetImageHandle(), look );
            const auto  radianceHandle = imageService->Register( std::move( radianceCube ),
                                                                 Runtime::ImageHandle::Type::ImageCube );

            // 2) Diffuse irradiance (from the panorama directly).
            auto       diffuseIrradiance       = CreateDiffuseIrradiance( imagePanorama->GetImageHandle(), look );
            const auto diffuseIrradianceHandle = imageService->Register(
                 std::move( diffuseIrradiance ), Runtime::ImageHandle::Type::ImageCube );

            // 3) Prefiltered specular (real GGX per-mip convolution of the radiance cube).
            auto       prefiltered       = CreatePrefilteredMap( radianceHandle );
            const auto prefilteredHandle = imageService->Register(
                 std::move( prefiltered ), Runtime::ImageHandle::Type::ImageCube );

            return { skyboxAsset->GetMetadata().Filepath, radianceHandle, diffuseIrradianceHandle,
                     prefilteredHandle };
        }

        return {};
    }

    std::shared_ptr<Desert::Graphic::ImageCube>
    EnvironmentManager::ConvertPanoramaToRadianceCube( const Runtime::ImageHandle& panorama, const SkyLook& look )
    {
        ComputeImagesSpecification processingInfo;
        processingInfo.InputHandle = panorama;
        processingInfo.Look        = look;
        processingInfo.ShaderName  = "PanoramaToCubemap";
        // The tag becomes the image's own name AND the compute pipeline's Vulkan debug label
        // (ComputeImages.cpp:103 and :116). It is NOT a cache key — PipelineCache deliberately does not
        // hash DebugName — so a shared name collides with nothing, which is exactly why the literal
        // "TODO" that used to sit here survived: it broke only the GPU capture, and only at the moment
        // someone was reading one. Two different compute dispatches both labelled "TODO" is the least
        // useful thing a capture of this 727 ms bake chain can say.
        processingInfo.Tag = "EnvRadiance";
        // The face sizes and mip counts live in SkyRules.hpp beside the cost report, because the report
        // has to compute the same numbers and a second copy of them is how a report starts lying — the
        // prefiltered cube below shipped at a QUARTER of its reported face exactly that way.
        processingInfo.FaceSize  = kSkyEnvCubeFaceSize;
        processingInfo.MipLevels = kSkyEnvRadianceMips;

        return ComputeImages::ProccessForImageCube( processingInfo );
    }

    std::shared_ptr<Desert::Graphic::ImageCube>
    EnvironmentManager::CreateDiffuseIrradiance( const Runtime::ImageHandle& panorama, const SkyLook& look )
    {
        ComputeImagesSpecification processingInfo;
        processingInfo.InputHandle = panorama;
        // THE SECOND HALF OF THE SAME ANSWER. This cube is convolved from the panorama directly, not
        // from the radiance cube, so it needs the look in its own right — and that is precisely the
        // asymmetry that would have let an authored rotation reach the visible sky and miss the ambient.
        processingInfo.Look        = look;
        processingInfo.ShaderName  = "DiffuseIrradiance";
        // Names the 301.8 ms stage of the bake — see the note in ConvertPanoramaToRadianceCube.
        processingInfo.Tag       = "EnvDiffuseIrradiance";
        processingInfo.FaceSize  = kSkyEnvIrradianceFaceSize;
        processingInfo.MipLevels = 1u;

        return ComputeImages::ProccessForImageCube( processingInfo );
    }

    Environment EnvironmentManager::CreateProcedural( uint32_t panoramaWidth, uint32_t panoramaHeight,
                                                      ShaderResources::StorageBuffer* skyParams,
                                                      Image2D* transmittanceLut, Image2D* multiScatterLut,
                                                      const CloudBakeBinding& clouds )
    {
        // Same reason as Create() above: a procedural sky's bake products have no file behind them at all.
        const ResourceAttributionScope owned( ResourceOwner::Environment );

        auto* imageService = Runtime::ResourceRegistry::GetImageService();

        // Bake the atmosphere AND the cloud layer standing in it into one equirect HDR panorama, then run
        // the standard IBL pipeline on it. The clouds go in HERE, at the producer, rather than into any of
        // the three stages below: the diffuse irradiance and the prefiltered specular both descend from
        // this one image, and giving them the clouds separately would be two sources of truth for one sky.
        auto panorama = ComputeImages::BakeProceduralPanorama( panoramaWidth, panoramaHeight, skyParams,
                                                               transmittanceLut, multiScatterLut, clouds );
        if ( !panorama )
            return {};
        const auto panoramaHandle =
             imageService->Register( std::move( panorama ), Runtime::ImageHandle::Type::Image2D );

        // 1) Radiance cube (sharp environment) — also the source the prefilter convolves.
        // THE IDENTITY LOOK, EXPLICITLY. A procedural sky is generated from its own authored parameters;
        // there is no file to rotate or grade, so the panorama it bakes is already the sky as asked for.
        // Named rather than defaulted so the asymmetry with the .hdr path above is visible here.
        auto       radianceCube   = ConvertPanoramaToRadianceCube( panoramaHandle, SkyLook{} );
        const auto radianceHandle = imageService->Register( std::move( radianceCube ),
                                                            Runtime::ImageHandle::Type::ImageCube );

        // 2) Diffuse irradiance (from the panorama directly).
        auto       diffuseIrradiance       = CreateDiffuseIrradiance( panoramaHandle, SkyLook{} );
        const auto diffuseIrradianceHandle = imageService->Register(
             std::move( diffuseIrradiance ), Runtime::ImageHandle::Type::ImageCube );

        // 3) Prefiltered specular (real GGX per-mip convolution of the radiance cube).
        auto       prefiltered       = CreatePrefilteredMap( radianceHandle );
        const auto prefilteredHandle = imageService->Register( std::move( prefiltered ),
                                                              Runtime::ImageHandle::Type::ImageCube );

        // The panorama was only an intermediate (consumed by the synchronous compute dispatches above).
        imageService->Unregister( panoramaHandle );

        return { Common::Filepath( "ProceduralSky" ), radianceHandle, diffuseIrradianceHandle,
                 prefilteredHandle };
    }

    std::shared_ptr<ImageCube>
    EnvironmentManager::CreatePrefilteredMap( const Runtime::ImageHandle& radianceCube )
    {
        // GGX per-mip convolution of the already-built radiance cube (roughness ramps with mip).
        ComputeImagesSpecification processingInfo;
        processingInfo.InputHandle = radianceCube;
        processingInfo.ShaderName  = "PrefilterEnvMap";
        processingInfo.Tag         = "EnvPrefiltered";
        processingInfo.FaceSize    = kSkyEnvPrefilterFaceSize;
        processingInfo.MipLevels   = kSkyEnvPrefilterMips;

        return ComputeImages::ProccessForImageCubeMips( processingInfo );
    }

} // namespace Desert::Graphic
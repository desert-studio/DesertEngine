#include <Engine/Graphic/Environment/SceneEnvironment.hpp>

#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ComputeImages.hpp>
#include <Engine/Graphic/SkyRules.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>

#include <chrono>

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

            // ── THE BAKED FORM, IF THERE IS ONE ──────────────────────────────────────────────────
            //
            // WHAT THIS REPLACES, AND WHY ONLY HERE. The three dispatches below are a function of the
            // panorama FILE and the authored look, and of nothing else — so on this path, and ONLY on
            // this path, they are work that can be done once. `CreateProcedural` looks identical three
            // lines at a time and must never grow this branch: its panorama is a function of the sun's
            // direction, so a cache of it is a cache of one instant of the day.
            //
            // A MISS IS A SENTENCE, NOT A SILENCE. Every refusal below is logged with its reason,
            // because "the cache never hits" and "the cache is never written" are indistinguishable
            // from the frame time and would each simply look like the old behaviour.
            // THE ONE NUMBER THIS WHOLE CHANGE IS ABOUT, PRINTED BY THE CODE THAT PAYS IT. A whole-frame
            // delta cannot see it — the bake is synchronous inside a scene load — and a figure quoted
            // from a document is a figure that was true on a different tree.
            const auto     startedAt       = std::chrono::steady_clock::now();
            const auto&    meta            = skyboxAsset->GetMetadata();
            const uint64_t sourceSignature = EnvironmentSourceSignature( meta.Filepath );
            const uint64_t radianceBake    = EnvironmentBakeSignature( look, BakedEnvironmentCube::Radiance,
                                                                       kSkyEnvCubeFaceSize, kSkyEnvRadianceMips );
            const uint64_t irradianceBake =
                 EnvironmentBakeSignature( look, BakedEnvironmentCube::Irradiance, kSkyEnvIrradianceFaceSize, 1u );
            const uint64_t prefilterBake = EnvironmentBakeSignature(
                 look, BakedEnvironmentCube::Prefiltered, kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips );

            const std::filesystem::path radiancePath   = EnvironmentBakePath( sourceSignature, radianceBake );
            const std::filesystem::path irradiancePath = EnvironmentBakePath( sourceSignature, irradianceBake );
            const std::filesystem::path prefilterPath  = EnvironmentBakePath( sourceSignature, prefilterBake );

            // ALL THREE OR NONE. Two cubes off the disk and one recomputed is a legal-looking
            // environment whose halves came from different bakes; the cheapest way to make that
            // impossible is to treat the trio as the unit it is.
            if ( sourceSignature != 0 )
            {
                auto cachedRadiance =
                     LoadBakedEnvironmentCube( radiancePath, "EnvRadiance", kSkyEnvCubeFaceSize,
                                               kSkyEnvRadianceMips, sourceSignature, radianceBake );
                auto cachedIrradiance =
                     LoadBakedEnvironmentCube( irradiancePath, "EnvDiffuseIrradiance", kSkyEnvIrradianceFaceSize,
                                               1u, sourceSignature, irradianceBake );
                auto cachedPrefilter =
                     LoadBakedEnvironmentCube( prefilterPath, "EnvPrefiltered", kSkyEnvPrefilterFaceSize,
                                               kSkyEnvPrefilterMips, sourceSignature, prefilterBake );

                if ( cachedRadiance.IsSuccess() && cachedIrradiance.IsSuccess() && cachedPrefilter.IsSuccess() )
                {
                    LOG_INFO(
                         "[SceneEnvironment] '{}' was loaded from its baked IBL chain in {:.1f} ms; the "
                         "three compute passes did not run.",
                         meta.Filepath.string(),
                         std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - startedAt )
                              .count() );
                    return { meta.Filepath,
                             imageService->Register( cachedRadiance.ExtractValue(),
                                                     Runtime::ImageHandle::Type::ImageCube ),
                             imageService->Register( cachedIrradiance.ExtractValue(),
                                                     Runtime::ImageHandle::Type::ImageCube ),
                             imageService->Register( cachedPrefilter.ExtractValue(),
                                                     Runtime::ImageHandle::Type::ImageCube ) };
                }

                LOG_INFO( "[SceneEnvironment] '{}' is being baked: {} / {} / {}", meta.Filepath.string(),
                          cachedRadiance.IsSuccess() ? std::string( "radiance ready" ) : cachedRadiance.GetError(),
                          cachedIrradiance.IsSuccess() ? std::string( "irradiance ready" )
                                                       : cachedIrradiance.GetError(),
                          cachedPrefilter.IsSuccess() ? std::string( "prefilter ready" )
                                                      : cachedPrefilter.GetError() );
            }

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

            // ── AND THEN IT IS WRITTEN, ONCE ─────────────────────────────────────────────────────
            //
            // The write happens AFTER the three cubes exist and reads them back off the device, so what
            // is cached is exactly what this run computed rather than a second computation of it. A
            // failure here costs the cache and nothing else: the environment in hand is complete, and
            // the next load simply bakes again with the reason in the log.
            if ( sourceSignature != 0 )
            {
                const std::string sourceKey = Common::AssetHandle::StableKeyForPath( meta.Filepath );
                const struct
                {
                    Runtime::ImageHandle         Handle;
                    const std::filesystem::path& Path;
                    uint64_t                     Bake;
                } toWrite[] = { { radianceHandle, radiancePath, radianceBake },
                                { diffuseIrradianceHandle, irradiancePath, irradianceBake },
                                { prefilteredHandle, prefilterPath, prefilterBake } };

                for ( const auto& entry : toWrite )
                {
                    auto* image = imageService->Resolve( entry.Handle );
                    auto* cube  = dynamic_cast<ImageCube*>( image );
                    if ( !cube )
                        continue;
                    if ( const auto written = WriteBakedEnvironmentCube( entry.Path, *cube, sourceKey,
                                                                         sourceSignature, entry.Bake );
                         !written )
                    {
                        LOG_ERROR( "[SceneEnvironment] '{}' was baked but not cached to '{}': {}",
                                   meta.Filepath.string(), entry.Path.string(), written.GetError() );
                    }
                }
            }

            LOG_INFO(
                 "[SceneEnvironment] '{}' computed its IBL chain in {:.1f} ms (radiance {}^2 x{}, "
                 "irradiance {}^2, prefilter {}^2 x{}) = {:.1f} MiB resident.",
                 meta.Filepath.string(),
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - startedAt ).count(),
                 kSkyEnvCubeFaceSize, kSkyEnvRadianceMips, kSkyEnvIrradianceFaceSize, kSkyEnvPrefilterFaceSize,
                 kSkyEnvPrefilterMips,
                 static_cast<double>( SkyEnvironmentCubeBytes( kSkyEnvCubeFaceSize, kSkyEnvRadianceMips ) +
                                      SkyEnvironmentCubeBytes( kSkyEnvIrradianceFaceSize, 1u ) +
                                      SkyEnvironmentCubeBytes( kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips ) ) /
                      ( 1024.0 * 1024.0 ) );

            return { meta.Filepath, radianceHandle, diffuseIrradianceHandle, prefilteredHandle };
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

        // AND SO WAS THE RADIANCE CUBE, ON THIS PATH ONLY. The sharp cube has exactly one consumer here
        // — `CreatePrefilteredMap` three lines up, which has already convolved it — and then nothing at
        // all. The procedural sky's BACKDROP is marched fullscreen: `SkyboxRenderer::Render` submits the
        // atmosphere quad and RETURNS before `MaterialSkybox::BindInputs`, the only code in the engine
        // that samples a radiance cube for the sky. What lights and reflects the world is the other two
        // — `SceneRenderer` and `MeshRenderer` bind `IrradianceMap` and `PreFilteredMap` by name and
        // never ask for this one. Measured: 100 663 296 B (96 MiB) per live environment, and six camera
        // points of Fog_Showcase (zenith x2, mid x2, horizon x2) are byte-identical without it.
        //
        // IT IS STILL BAKED, and that is not a detail to optimise away next: `PrefilterEnvMap.shader`
        // declares `Uniform(0,0) samplerCube inputTexture` and this cube is its only input. Transient,
        // not absent.
        //
        // THE HANDLE IS CLEARED RATHER THAN CARRIED. `ImageService::Resolve` is generation-checked, so a
        // handle whose image has been unregistered answers nullptr and never somebody else's image — but
        // an Environment that NAMES a radiance cube is making a claim its consumers are entitled to
        // believe, and `Unregister` would report the stale handle as an error on the next rebake. Absent
        // is the honest value, and `operator bool` below no longer asks for it.
        //
        // `EnvironmentManager::Create` (the .hdr path) deliberately does NOT do this: there the cube is
        // the backdrop the skybox pass draws and the image both editor previews read, and the two
        // measured substitutes for it both lose — prefilter mip 0 is visibly blockier (max delta 123/255
        // at the zenith on real content) and the panorama sampled directly crawls under motion (rms
        // 14.33 vs 10.60 under a 0.40 deg camera nudge, coherence 1.49 — per-pixel speckle).
        imageService->Unregister( radianceHandle );

        return { Common::Filepath( "ProceduralSky" ), Runtime::ImageHandle{}, diffuseIrradianceHandle,
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
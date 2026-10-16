#include <Engine/Graphic/Environment/SceneEnvironment.hpp>

#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ComputeImages.hpp>
#include <Engine/Graphic/GpuBatch.hpp>
#include <Engine/Graphic/SkyRules.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>

#include <chrono>

namespace Desert::Graphic
{
    Environment EnvironmentManager::Create( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset,
                                            std::unique_ptr<GpuBatch>&                  convolving )
    {
        convolving.reset();
        // The panorama, the three cubes and the transient compute pipelines the bake creates are all the
        // ENVIRONMENT's, not the skybox asset's: the recipe that rebuilds them is the sky settings plus a
        // 450 ms bake, not a file read, so eviction must never treat them as reloadable. One scope rather
        // than five claims — see Engine/Graphic/ResourceLedger.hpp on ResourceAttributionScope.
        const ResourceAttributionScope owned( ResourceOwner::Environment );

        auto* imageService = Runtime::ResourceRegistry::GetImageService();

        // ── THE BAKED FORM, IF THERE IS ONE ──────────────────────────────────────────────────
        //
        // WHAT THIS REPLACES, AND WHY ONLY HERE. The three dispatches below are a function of the
        // panorama FILE and of nothing else — the authored look is applied where the cubes are
        // sampled — so on this path, and ONLY on this path, they are work that can be done once.
        // The procedural sky capture (SkyCapture) runs the same programs and must never grow this branch: its
        // panorama is a function of the sun's direction, so a cache of it is a cache of one instant of the
        // day.
        //
        // A MISS IS A SENTENCE, NOT A SILENCE. Every refusal below is logged with its reason,
        // because "the cache never hits" and "the cache is never written" are indistinguishable
        // from the frame time and would each simply look like the old behaviour.
        // THE ONE NUMBER THIS WHOLE CHANGE IS ABOUT, PRINTED BY THE CODE THAT PAYS IT. A whole-frame
        // delta cannot see it — the bake is synchronous inside a scene load — and a figure quoted
        // from a document is a figure that was true on a different tree.
        const auto  startedAt = std::chrono::steady_clock::now();
        const auto& meta      = skyboxAsset->GetMetadata();

        // THE PANORAMA IS A COOKED TEXTURE, AND IT IS ASKED FOR BEFORE ANYTHING IS DECODED. This used
        // to begin with `Texture2D::Create( {true}, <the .hdr> )`, which decoded the SOURCE through
        // stb ahead of the cache check below: on a hit that decode was thrown away unread, and on a
        // miss it was the runtime holding an image decoder. The cook (`TextureImporter`, run over
        // `Assets/Textures/` at every editor start) writes the panorama's `.tex` and records the
        // source signature in its header, so the key below needs one prefix read and no source.
        //
        // A MISSING COOK IS AN EMPTY ENVIRONMENT, LOUDLY — the same shape a panorama that did not
        // load has always produced here, so the caller needs nothing new.
        //
        // THE ASSET'S HEADER DECIDES WHAT THIS SKYBOX IS, NOT ITS FILE NAME. This whole body used to sit
        // under `extension == ".hdr"` with a bare `return {};` after it; when AF3 moved every skybox to a
        // `.detex` container, every HDR sky fell through that return silently — black sky, no IBL, and
        // not one log line. `FindCookedPanorama` reads the container's own kind and key and names the
        // reason when it refuses, so it is the only gate.
        // Staged on the loader's worker (`SkyboxAsset::LoadFromFile`): the panorama's header, the cache
        // paths, and on a hit the three decoded cubes. Only the device work is left for this thread.
        const std::shared_ptr<const Assets::StagedEnvironment> staged = skyboxAsset->Staged();
        if ( !staged )
        {
            LOG_ERROR( "[SceneEnvironment] the skybox '{}' was handed over without being loaded (no staged "
                       "environment), so it gets NO environment -- request it through SkyboxService.",
                       meta.Filepath.string() );
            return {};
        }
        if ( !staged->Error.empty() )
        {
            LOG_ERROR( "[SceneEnvironment] the skybox '{}' gets NO environment (no radiance, no irradiance, "
                       "no prefiltered specular): {}",
                       meta.Filepath.string(), staged->Error );
            return {};
        }
        const std::filesystem::path& panoramaPath    = staged->Panorama.Path;
        const uint64_t               sourceSignature = staged->Panorama.SourceSignature;
        const std::filesystem::path& radiancePath    = staged->RadiancePath;
        const std::filesystem::path& irradiancePath  = staged->IrradiancePath;
        const std::filesystem::path& prefilterPath   = staged->PrefilterPath;
        const uint64_t               radianceBake    = staged->RadianceBake;
        const uint64_t               irradianceBake  = staged->IrradianceBake;
        const uint64_t               prefilterBake   = staged->PrefilterBake;

        if ( const auto& cached = staged->Cached; cached.has_value() )
        {
            const auto& cubes      = *cached;
            auto        radiance   = CreateBakedEnvironmentCube( cubes[0], radiancePath );
            auto        irradiance = CreateBakedEnvironmentCube( cubes[1], irradiancePath );
            auto        prefilter  = CreateBakedEnvironmentCube( cubes[2], prefilterPath );
            if ( radiance.IsSuccess() && irradiance.IsSuccess() && prefilter.IsSuccess() )
            {
                LOG_INFO( "[SceneEnvironment] '{}' was loaded from its baked IBL chain: {:.1f} ms read on the "
                          "loader's worker, {:.1f} ms uploading here; the panorama was not read and the three "
                          "compute passes did not run.",
                          meta.Filepath.string(), staged->StageMs,
                          std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - startedAt )
                               .count() );
                return {
                     meta.Filepath,
                     imageService->Register( radiance.ExtractValue(), Runtime::ImageHandle::Type::ImageCube ),
                     imageService->Register( irradiance.ExtractValue(), Runtime::ImageHandle::Type::ImageCube ),
                     imageService->Register( prefilter.ExtractValue(), Runtime::ImageHandle::Type::ImageCube ) };
            }
            LOG_ERROR( "[SceneEnvironment] '{}' decoded its baked IBL chain but a GPU cube was not created ({} / "
                       "{} / {}); baking instead.",
                       meta.Filepath.string(), radiance.IsSuccess() ? "radiance ok" : radiance.GetError(),
                       irradiance.IsSuccess() ? "irradiance ok" : irradiance.GetError(),
                       prefilter.IsSuccess() ? "prefilter ok" : prefilter.GetError() );
        }
        else
        {
            LOG_INFO( "[SceneEnvironment] '{}' is being baked: {}", meta.Filepath.string(), staged->MissReason );
        }

        auto panorama = Texture2D::CreateFromAsset( panoramaPath );
        if ( !panorama )
        {
            LOG_ERROR( "[SceneEnvironment] the cooked panorama '{}' of skybox '{}' did not load, so this "
                       "scene gets NO environment: {}",
                       panoramaPath.string(), meta.Filepath.string(), panorama.GetError() );
            return {};
        }
        const std::shared_ptr<Texture2D> imagePanorama = panorama.ExtractValue();
        const auto                       uploadedAt    = std::chrono::steady_clock::now();

        // ONE BATCH, SUBMITTED AND NOT WAITED FOR (AL1-3c). The three passes below used to be eleven
        // one-off buffers this thread slept on in turn — 211 ms of a scene load. Recorded into one buffer
        // on the graphics queue, their only order is the barriers between them, and the cache readbacks
        // submitted after it on the same queue need nothing more. The caller keeps the batch and holds
        // the skybox pending until its fence; the panorama and the three cubes are retained by the batch,
        // so neither an unregistered handle nor a dropped skybox can free an image the GPU is still using.
        auto begun = GpuBatch::Begin();
        if ( !begun )
        {
            LOG_ERROR( "[SceneEnvironment] '{}' gets NO environment: the convolution batch was not begun: {}",
                       meta.Filepath.string(), begun.GetError() );
            return {};
        }
        std::unique_ptr<GpuBatch> batch = begun.ExtractValue();
        batch->Retain( imagePanorama );

        // 1) Radiance cube (sharp environment) — also the source the prefilter convolves.
        auto radianceCube = ConvertPanoramaToRadianceCube( *batch, imagePanorama->GetImageHandle() );
        batch->Retain( radianceCube );
        const auto radianceHandle =
             imageService->Register( std::move( radianceCube ), Runtime::ImageHandle::Type::ImageCube );

        // 2) Diffuse irradiance, summed exactly over the radiance cube's 16-face mip (the procedural capture's
        //    estimator, SkyCaptureIrradiance).
        auto diffuseIrradiance = CreateDiffuseIrradiance( *batch, radianceHandle );
        batch->Retain( diffuseIrradiance );
        const auto diffuseIrradianceHandle =
             imageService->Register( std::move( diffuseIrradiance ), Runtime::ImageHandle::Type::ImageCube );

        // 3) Prefiltered specular (real GGX per-mip convolution of the radiance cube).
        auto prefiltered = CreatePrefilteredMap( *batch, radianceHandle );
        batch->Retain( prefiltered );
        const auto prefilteredHandle =
             imageService->Register( std::move( prefiltered ), Runtime::ImageHandle::Type::ImageCube );

        if ( const auto submitted = batch->Submit(); !submitted )
        {
            // Nothing will ever write these three, so none of them may be handed out.
            for ( const auto& unwritten : { radianceHandle, diffuseIrradianceHandle, prefilteredHandle } )
                imageService->Unregister( unwritten );
            LOG_ERROR( "[SceneEnvironment] '{}' gets NO environment: its convolution batch was not submitted: {}",
                       meta.Filepath.string(), submitted.GetError() );
            return {};
        }

        // The halves of a miss are timed apart because they answer different questions: the panorama
        // upload is still a waited copy, the recording is CPU only, the last part SUBMITS the cache's readbacks.
        const auto convolvedAt = std::chrono::steady_clock::now();

        // ── AND THEN IT IS WRITTEN, ONCE, AND NOT HERE ───────────────────────────────────────
        //
        // The write reads the three cubes back off the device, so what is cached is exactly what this run
        // computed. Nothing here waits for it, and when it lands the writer puts the cube read back OUT OF
        // THE FILE behind each handle below, so this run ends up drawing exactly what the next run loads
        // (ENV-FIRST1: BC6H clamps above 65504 and quantises; RGBA32F does not). `EnvironmentCacheWriter` submits
        // the copies, polls their fence once a tick and encodes BC6H + writes the file on a worker — that was 14.7
        // s of the main thread on a cold open (AL1-3). A failure costs the cache and nothing else, and is logged
        // with the file by the writer when it lands.
        {
            const std::string sourceKey = Common::AssetHandle::StableKeyForPath( meta.Filepath );
            const struct
            {
                Runtime::ImageHandle         Handle;
                const std::filesystem::path& Path;
                uint64_t                     Bake = 0;
                std::string_view             Tag;
            } toWrite[] = { { radianceHandle, radiancePath, radianceBake, Assets::kEnvRadianceTag },
                            { diffuseIrradianceHandle, irradiancePath, irradianceBake, Assets::kEnvIrradianceTag },
                            { prefilteredHandle, prefilterPath, prefilterBake, Assets::kEnvPrefilterTag } };

            for ( const auto& entry : toWrite )
            {
                auto* image = imageService->Resolve( entry.Handle );
                auto* cube  = dynamic_cast<ImageCube*>( image );
                if ( cube == nullptr )
                    continue;
                if ( const auto begun =
                          EnvironmentCacheWriter::Get().Begin( *cube, { .Path            = entry.Path,
                                                                        .SourceKey       = sourceKey,
                                                                        .SourceSignature = sourceSignature,
                                                                        .BakeSignature   = entry.Bake,
                                                                        .Tag  = std::string( entry.Tag ),
                                                                        .Live = entry.Handle } );
                     !begun )
                {
                    LOG_ERROR( "[SceneEnvironment] '{}' was baked but will not be cached to '{}': {}",
                               meta.Filepath.string(), entry.Path.string(), begun.GetError() );
                }
            }
        }

        LOG_INFO(
             "[SceneEnvironment] '{}' submitted its IBL chain in {:.1f} ms of this thread ({:.1f} ms uploading "
             "the "
             "panorama, {:.1f} ms recording and submitting the convolutions, {:.1f} ms "
             "submitting the cache readbacks); the GPU convolves it after this returns (radiance {}^2 x{}, "
             "irradiance {}^2, prefilter {}^2 x{}) = {:.1f} MiB resident.",
             meta.Filepath.string(),
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - startedAt ).count(),
             std::chrono::duration<double, std::milli>( uploadedAt - startedAt ).count(),
             std::chrono::duration<double, std::milli>( convolvedAt - uploadedAt ).count(),
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - convolvedAt ).count(),
             kSkyEnvCubeFaceSize, kSkyEnvRadianceMips, kSkyEnvIrradianceFaceSize, kSkyEnvPrefilterFaceSize,
             kSkyEnvPrefilterMips,
             static_cast<double>( SkyEnvironmentCubeBytes( kSkyEnvCubeFaceSize, kSkyEnvRadianceMips ) +
                                  SkyEnvironmentCubeBytes( kSkyEnvIrradianceFaceSize, 1u ) +
                                  SkyEnvironmentCubeBytes( kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips ) ) /
                  ( 1024.0 * 1024.0 ) );

        convolving = std::move( batch );
        return { meta.Filepath, radianceHandle, diffuseIrradianceHandle, prefilteredHandle };
    }

    std::shared_ptr<Desert::Graphic::ImageCube>
    EnvironmentManager::ConvertPanoramaToRadianceCube( GpuBatch& batch, const Runtime::ImageHandle& panorama )
    {
        ComputeImagesSpecification processingInfo;
        processingInfo.InputHandle = panorama;
        processingInfo.ShaderName  = "PanoramaToCubemap";
        // The tag becomes the image's own name AND the compute pipeline's Vulkan debug label
        // (ComputeImages.cpp:103 and :116). It is NOT a cache key — PipelineCache deliberately does not
        // hash DebugName — so a shared name collides with nothing, which is exactly why the placeholder
        // label that used to sit here survived: it broke only the GPU capture, and only at the moment
        // someone was reading one. Two different compute dispatches under one placeholder label is the
        // least useful thing a capture of this 727 ms bake chain can say.
        processingInfo.Tag = "EnvRadiance";
        // The face sizes and mip counts live in SkyRules.hpp beside the cost report, because the report
        // has to compute the same numbers and a second copy of them is how a report starts lying — the
        // prefiltered cube below shipped at a QUARTER of its reported face exactly that way.
        processingInfo.FaceSize  = kSkyEnvCubeFaceSize;
        processingInfo.MipLevels = kSkyEnvRadianceMips;

        return ComputeImages::ProccessForImageCube( batch, processingInfo );
    }

    std::shared_ptr<Desert::Graphic::ImageCube>
    EnvironmentManager::CreateDiffuseIrradiance( GpuBatch& batch, const Runtime::ImageHandle& radianceCube )
    {
        // ONE ESTIMATOR FOR BOTH PRODUCERS: the exact sum over the radiance cube's 16-face mip, the program the
        // procedural sky's capture records (SkyCapture::AddIrradiance). It replaced a 65 536-sample Monte Carlo
        // over the panorama (DiffuseIrradiance, 301.8 ms of this bake), whose noise the cached file kept.
        ComputeImagesSpecification processingInfo;
        processingInfo.InputHandle = radianceCube;
        processingInfo.ShaderName  = "SkyCaptureIrradiance";
        processingInfo.Tag         = "EnvDiffuseIrradiance";
        processingInfo.FaceSize    = kSkyEnvIrradianceFaceSize;
        processingInfo.MipLevels   = 1u;

        return ComputeImages::IntegrateIrradianceCube( batch, processingInfo, kSkyEnvIrradianceSourceMip );
    }

    std::shared_ptr<ImageCube> EnvironmentManager::CreatePrefilteredMap( GpuBatch&                   batch,
                                                                         const Runtime::ImageHandle& radianceCube )
    {
        // GGX per-mip convolution of the already-built radiance cube (roughness ramps with mip).
        ComputeImagesSpecification processingInfo;
        processingInfo.InputHandle = radianceCube;
        processingInfo.ShaderName  = "PrefilterEnvMap";
        processingInfo.Tag         = "EnvPrefiltered";
        processingInfo.FaceSize    = kSkyEnvPrefilterFaceSize;
        processingInfo.MipLevels   = kSkyEnvPrefilterMips;

        return ComputeImages::ProccessForImageCubeMips( batch, processingInfo );
    }

} // namespace Desert::Graphic
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
        // `CreateProcedural` looks identical three lines at a time and must never grow this branch: its
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

        // 2) Diffuse irradiance (from the panorama directly).
        auto diffuseIrradiance = CreateDiffuseIrradiance( *batch, imagePanorama->GetImageHandle() );
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

        return ComputeImages::ProccessForImageCube( batch, processingInfo );
    }

    std::shared_ptr<Desert::Graphic::ImageCube>
    EnvironmentManager::CreateDiffuseIrradiance( GpuBatch& batch, const Runtime::ImageHandle& panorama )
    {
        ComputeImagesSpecification processingInfo;
        processingInfo.InputHandle = panorama;
        processingInfo.ShaderName  = "DiffuseIrradiance";
        // Names the 301.8 ms stage of the bake — see the note in ConvertPanoramaToRadianceCube.
        processingInfo.Tag       = "EnvDiffuseIrradiance";
        processingInfo.FaceSize  = kSkyEnvIrradianceFaceSize;
        processingInfo.MipLevels = 1u;

        return ComputeImages::ProccessForImageCube( batch, processingInfo );
    }

    ProceduralEnvironmentBake::~ProceduralEnvironmentBake()
    {
        ReleaseAll();
    }

    bool ProceduralEnvironmentBake::IsComplete() const
    {
        return !m_Batch || m_Batch->IsComplete();
    }

    void ProceduralEnvironmentBake::Wait()
    {
        if ( m_Batch )
            m_Batch->Wait();
    }

    void ProceduralEnvironmentBake::ReleaseAll()
    {
        // The batch first: its destructor waits for the GPU, and only then may the images it writes go.
        m_Batch.reset();
        auto* imageService = Runtime::ResourceRegistry::GetImageService();
        for ( Runtime::ImageHandle* handle : { &m_Panorama, &m_Radiance, &m_Irradiance, &m_Prefiltered } )
        {
            if ( handle->IsValid() )
                imageService->Unregister( *handle );
            *handle = {};
        }
    }

    Environment ProceduralEnvironmentBake::Finish()
    {
        if ( !IsComplete() )
        {
            LOG_ERROR( "[SceneEnvironment] a procedural sky bake was finished before the GPU had written it; "
                       "nothing is handed over." );
            return {};
        }
        m_Batch.reset();

        auto* imageService = Runtime::ResourceRegistry::GetImageService();

        // The panorama was only an intermediate (consumed by the dispatches recorded after it).
        if ( m_Panorama.IsValid() )
            imageService->Unregister( m_Panorama );
        m_Panorama = {};

        // AND SO WAS THE RADIANCE CUBE, ON THIS PATH ONLY. The sharp cube has exactly one consumer here
        // — `CreatePrefilteredMap` in `BeginProcedural`, which has already convolved it — and then nothing at
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
        if ( m_Radiance.IsValid() )
            imageService->Unregister( m_Radiance );
        m_Radiance = {};

        Environment baked{ Common::Filepath( "ProceduralSky" ), Runtime::ImageHandle{}, m_Irradiance,
                           m_Prefiltered };
        m_Irradiance  = {};
        m_Prefiltered = {};
        return baked;
    }

    Environment EnvironmentManager::CreateProcedural( uint32_t panoramaWidth, uint32_t panoramaHeight,
                                                      ShaderResources::StorageBuffer* skyParams,
                                                      Image2D* transmittanceLut, Image2D* multiScatterLut,
                                                      const CloudBakeBinding& clouds )
    {
        const auto bake = BeginProcedural( panoramaWidth, panoramaHeight, skyParams, transmittanceLut,
                                           multiScatterLut, clouds );
        if ( !bake )
            return {};
        bake->Wait();
        return bake->Finish();
    }

    std::unique_ptr<ProceduralEnvironmentBake>
    EnvironmentManager::BeginProcedural( uint32_t panoramaWidth, uint32_t panoramaHeight,
                                         ShaderResources::StorageBuffer* skyParams, Image2D* transmittanceLut,
                                         Image2D* multiScatterLut, const CloudBakeBinding& clouds )
    {
        // Same reason as Create() above: a procedural sky's bake products have no file behind them at all.
        const ResourceAttributionScope owned( ResourceOwner::Environment );

        auto* imageService = Runtime::ResourceRegistry::GetImageService();

        // ONE BATCH FOR THE WHOLE CHAIN, panorama included, and NOT WAITED: the owner polls it. The
        // dispatches are ordered inside the buffer by the barriers each records, and the frames that sample
        // the cubes go to the same queue after it, so submission order is the only cross-batch dependency.
        auto begun = GpuBatch::Begin();
        if ( !begun )
        {
            LOG_ERROR( "[SceneEnvironment] the procedural sky gets NO environment: {}", begun.GetError() );
            return nullptr;
        }
        auto bake       = std::make_unique<ProceduralEnvironmentBake>();
        bake->m_Batch   = begun.ExtractValue();
        GpuBatch& batch = *bake->m_Batch;

        // Bake the atmosphere AND the cloud layer standing in it into one equirect HDR panorama, then run
        // the standard IBL pipeline on it. The clouds go in HERE, at the producer, rather than into any of
        // the three stages below: the diffuse irradiance and the prefiltered specular both descend from
        // this one image, and giving them the clouds separately would be two sources of truth for one sky.
        auto panorama = ComputeImages::BakeProceduralPanorama( batch, panoramaWidth, panoramaHeight, skyParams,
                                                               transmittanceLut, multiScatterLut, clouds );
        if ( !panorama )
            return nullptr; // the bake's destructor waits for the (empty) batch and releases nothing
        bake->m_Panorama = imageService->Register( std::move( panorama ), Runtime::ImageHandle::Type::Image2D );

        // 1) Radiance cube (sharp environment) — the source the prefilter convolves. The identity look: a
        // procedural sky is generated from its own authored parameters, there is no file to rotate or grade.
        bake->m_Radiance = imageService->Register( ConvertPanoramaToRadianceCube( batch, bake->m_Panorama ),
                                                   Runtime::ImageHandle::Type::ImageCube );

        // 2) Diffuse irradiance (from the panorama directly).
        bake->m_Irradiance = imageService->Register( CreateDiffuseIrradiance( batch, bake->m_Panorama ),
                                                     Runtime::ImageHandle::Type::ImageCube );

        // 3) Prefiltered specular (real GGX per-mip convolution of the radiance cube).
        bake->m_Prefiltered = imageService->Register( CreatePrefilteredMap( batch, bake->m_Radiance ),
                                                      Runtime::ImageHandle::Type::ImageCube );

        if ( const auto submitted = batch.Submit(); !submitted )
        {
            LOG_ERROR( "[SceneEnvironment] the procedural sky's bake batch was not submitted, so it gets NO "
                       "environment: {}",
                       submitted.GetError() );
            return nullptr;
        }
        return bake;
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
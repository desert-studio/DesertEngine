#include "SkyboxRenderer.hpp"
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SkyGroundTransmittance.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <chrono>

namespace Desert::Graphic::System
{
    namespace
    {
        // LUT extents from the paper (and UE's defaults): transmittance 256x64, multi-scattering 32x32,
        // Sky-View 192x104 (kSkyViewLutWidth/Height in SkyPayload.hpp — the shaders mirror that pair).
        // Not authorable — the sizes are part of the parameterisation the shaders and the SkyMedium /
        // SkyScattering tests share, and ~292 KiB total leaves nothing worth a quality dial.
        constexpr uint32_t kTransmittanceLutWidth  = 256;
        constexpr uint32_t kTransmittanceLutHeight = 64;
        constexpr uint32_t kMultiScatterLutSize    = 32;

        constexpr uint32_t kLutWorkGroupSize = 8; // LocalSize(8, 8, 1) in both LUT shaders

        constexpr uint32_t LutGroupCount( uint32_t extent )
        {
            return ( extent + kLutWorkGroupSize - 1 ) / kLutWorkGroupSize;
        }

        double LutBytesToMiB( uint64_t bytes )
        {
            return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
        }
    } // namespace

    Common::BoolResultStr SkyboxRenderer::Initialize()
    {
        const auto& compositeFramebuffer = m_TargetFramebuffer.lock();
        if ( !compositeFramebuffer )
        {
            DESERT_VERIFY( false );
        }

        constexpr std::string_view debugName = "Skybox";

        // RenderPass
        RenderPassSpecification rpSpec;
        rpSpec.DebugName         = debugName;
        rpSpec.TargetFramebuffer = compositeFramebuffer;

        // Pipeline
        m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Skybox" );

        Graphic::GraphicsPipelineSpecification pipeSpec;
        pipeSpec.DebugName   = debugName;
        pipeSpec.Framebuffer = compositeFramebuffer;
        pipeSpec.Shader      = m_Shader;

        pipeSpec.CullMode          = CullMode::None;
        pipeSpec.DepthTestEnabled  = false;
        pipeSpec.DepthWriteEnabled = false;

        // Same unchecked GetByName as Tonemap/FXAA: a missing 'Skybox' shader was a null dereference.
        const auto pipeline = Graphic::GraphicsPipeline::Create( pipeSpec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        // Procedural sky: same fullscreen-quad pass/target, but the engine-generated atmosphere shader.
        m_ProceduralShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "ProceduralSky" );
        Common::ResultStr<std::shared_ptr<Graphic::GraphicsPipeline>> proceduralPipeline;
        if ( m_ProceduralShader )
        {
            Graphic::GraphicsPipelineSpecification skySpec;
            skySpec.DebugName         = "ProceduralSky";
            skySpec.Framebuffer       = compositeFramebuffer;
            skySpec.Shader            = m_ProceduralShader;
            skySpec.CullMode          = CullMode::None;
            skySpec.DepthTestEnabled  = false;
            skySpec.DepthWriteEnabled = false;

            proceduralPipeline = Graphic::GraphicsPipeline::Create( skySpec );
            if ( !proceduralPipeline )
            {
                // The procedural sky is optional (a scene on a cubemap never asks for it), so a refusal
                // is not an Initialize failure — but everything below this point exists only to feed
                // this pipeline, so it is all skipped rather than half-built.
                LOG_ERROR( "[SkyAtmosphere] the procedural sky will not draw for this view: {}",
                           proceduralPipeline.GetError() );
            }
        }
        if ( proceduralPipeline )
        {
            m_ProceduralPipeline = proceduralPipeline.GetValue();

            m_ProceduralMaterial = std::make_shared<MaterialProceduralSky>();

            // Created here, not through shader reflection: the reflection path allocates a fixed 36 bytes,
            // which is not this block. persistent = false is load-bearing — it gives the backend one copy
            // per (frame in flight x renderer slot), which is what keeps a second live SceneRenderer (the
            // mesh preview, a thumbnail, another scene view) from overwriting this one's sky.
            m_SkyParams = ShaderResources::StorageBuffer::Create( "SkyBuffer", kSkyPayloadBytes,
                                                                  kSkyPayloadBinding, /*persistent=*/false );

            // The cloud layer's two blocks, for the environment bake alone — see the members' declaration
            // for why the bytes are the cloud renderer's and the buffers are this one's. Created here
            // rather than lazily so that "the scene has clouds" and "the bake can carry them" are never
            // two different answers: a scene that has none writes a zero payload it never reads, which is
            // 268 bytes per frame in flight and no dispatch at all.
            m_CloudBakeParams = ShaderResources::StorageBuffer::Create(
                 "SkyBakeCloudParams", kCloudPayloadBytes, kSkyBakeCloudParamsBinding, /*persistent=*/false );
            m_CloudBakeAuthored = ShaderResources::StorageBuffer::Create(
                 "SkyBakeCloudAuthored", static_cast<uint32_t>( sizeof( CloudAuthoredPayload ) ),
                 kSkyBakeCloudAuthoredBinding, /*persistent=*/false );
            m_CloudBakeMediumParams = ShaderResources::StorageBuffer::Create(
                 "SkyBakeCloudMediumParams", Core::kCloudMediumParamsBytes, Core::kCloudMediumParamsBinding,
                 /*persistent=*/false );

            // The physical atmosphere's LUT pipelines. Built up front (they are two small compute
            // pipelines); the IMAGES stay lazy, so a scene on
            // the artistic gradient allocates nothing.
            const auto makeCompute = [shaderService = Runtime::ResourceRegistry::GetShaderService()](
                                          const char* name ) -> std::shared_ptr<ComputePipeline>
            {
                const auto shader = shaderService->GetByName( name );
                if ( !shader )
                {
                    LOG_ERROR( "[SkyAtmosphere] Compute shader '{}' is not registered — expected "
                               "Editor/Resources/Shaders/Programs/Sky/{}.shader. The physical "
                               "atmosphere's LUTs will not be built for this view.",
                               name, name );
                    return nullptr;
                }
                // Create() hands back a BUILT pipeline or the reason it refused. The two-line idiom this
                // replaces — allocate here, `Invalidate()` on the next line — is what let a shader with
                // no compiled stages reach vkCreateComputePipelines and take the editor down.
                const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
                if ( !built )
                {
                    LOG_ERROR( "[SkyAtmosphere] {} The physical atmosphere's LUTs will not be built for "
                               "this view.",
                               built.GetError() );
                    return nullptr;
                }
                return built.GetValue();
            };

            m_TransmittanceLutPipeline  = makeCompute( "SkyTransmittanceLut" );
            m_MultiScatterLutPipeline   = makeCompute( "SkyMultiScatterLut" );
            m_SkyViewLutPipeline        = makeCompute( "SkyViewLut" );
            m_AerialPerspectivePipeline = makeCompute( "SkyAerialPerspectiveLut" );
            m_DistantLightPipeline      = makeCompute( "SkyDistantLight" );
        }

        return BOOLSUCCESS;
    }

    SkyboxRenderer::AtmosphereLutFingerprint SkyboxRenderer::LutFingerprintOf( const SkySettings& sky )
    {
        return AtmosphereLutFingerprint{ .RayleighScattering        = sky.RayleighScattering,
                                         .RayleighExpDistributionKm = sky.RayleighExpDistributionKm,
                                         .MieScattering             = sky.MieScattering,
                                         .MieAbsorption             = sky.MieAbsorption,
                                         .MieExpDistributionKm      = sky.MieExpDistributionKm,
                                         .OzoneAbsorption           = sky.OzoneAbsorption,
                                         .OzoneTipAltitudeKm        = sky.OzoneTipAltitudeKm,
                                         .OzoneTipValue             = sky.OzoneTipValue,
                                         .OzoneTentWidthKm          = sky.OzoneTentWidthKm,
                                         .GroundAlbedo              = sky.GroundAlbedo,
                                         .AtmosphereHeightKm        = sky.AtmosphereHeightKm,
                                         .MultiScatteringFactor     = sky.MultiScatteringFactor,
                                         .PlanetRadius              = sky.PlanetRadius };
    }

    bool SkyboxRenderer::EnsureAtmosphereLutResources()
    {
        if ( m_LutResourcesFailed )
            return false;
        if ( m_TransmittanceLut && m_MultiScatterLut )
            return true;

        const Core::Formats::Image2DSpecification transmittanceSpec{
             .Tag        = "SkyTransmittanceLut",
             .Width      = kTransmittanceLutWidth,
             .Height     = kTransmittanceLutHeight,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_TransmittanceLut = Image2D::Create( transmittanceSpec, nullptr );

        const Core::Formats::Image2DSpecification multiScatterSpec{
             .Tag        = "SkyMultiScatterLut",
             .Width      = kMultiScatterLutSize,
             .Height     = kMultiScatterLutSize,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_MultiScatterLut = Image2D::Create( multiScatterSpec, nullptr );

        if ( !m_TransmittanceLut || !m_MultiScatterLut )
        {
            LOG_ERROR( "[SkyAtmosphere] The atmosphere LUTs could not be created (transmittance "
                       "{}x{}: {}, multi-scattering {}x{}: {}); the physical atmosphere will not be "
                       "built for this view.",
                       kTransmittanceLutWidth, kTransmittanceLutHeight, m_TransmittanceLut ? "ok" : "FAILED",
                       kMultiScatterLutSize, kMultiScatterLutSize, m_MultiScatterLut ? "ok" : "FAILED" );
            m_TransmittanceLut.reset();
            m_MultiScatterLut.reset();
            m_LutResourcesFailed = true;
            return false;
        }

        LOG_INFO(
             "[SkyAtmosphere] Transmittance LUT {}x{} + multi-scattering LUT {}x{} RGBA16F "
             "({:.2f} MiB) allocated for the physical atmosphere.",
             kTransmittanceLutWidth, kTransmittanceLutHeight, kMultiScatterLutSize, kMultiScatterLutSize,
             LutBytesToMiB( Core::Formats::CalculateImageSize( kTransmittanceLutWidth, kTransmittanceLutHeight,
                                                               Core::Formats::ImageFormat::RGBA16F ) +
                            Core::Formats::CalculateImageSize( kMultiScatterLutSize, kMultiScatterLutSize,
                                                               Core::Formats::ImageFormat::RGBA16F ) ) );
        return true;
    }

    void SkyboxRenderer::DispatchCachedAtmosphereLuts( bool inFrame )
    {
        auto& renderer = Renderer::GetInstance();

        // Transmittance strictly first: the multi-scattering march samples it per step.
        m_TransmittanceLutPipeline->SetOutput( kSkyTransmittanceLutOutputBinding, m_TransmittanceLut.get(), 0 );
        m_TransmittanceLutPipeline->SetStorageBuffer( kSkyPayloadBinding, m_SkyParams.get() );
        if ( inFrame )
        {
            renderer.ComputeImageBeginWrite( m_TransmittanceLut.get() );
            renderer.DispatchComputeInFrame( m_TransmittanceLutPipeline.get(),
                                             LutGroupCount( kTransmittanceLutWidth ),
                                             LutGroupCount( kTransmittanceLutHeight ), 1 );
            renderer.ComputeImageEndWrite( m_TransmittanceLut.get() );
        }
        else
        {
            // Immediate submit (the bake path, which runs OUTSIDE a frame): ComputePipeline::Dispatch
            // owns the output's layout round-trip and leaves it sampleable, the same contract
            // EndWrite provides in-frame.
            m_TransmittanceLutPipeline->Dispatch( LutGroupCount( kTransmittanceLutWidth ),
                                                  LutGroupCount( kTransmittanceLutHeight ), 1 );
        }

        m_MultiScatterLutPipeline->SetOutput( kSkyMultiScatterLutOutputBinding, m_MultiScatterLut.get(), 0 );
        m_MultiScatterLutPipeline->SetStorageBuffer( kSkyPayloadBinding, m_SkyParams.get() );
        // Written a moment ago; both paths leave it sampleable by the dispatch that follows.
        m_MultiScatterLutPipeline->SetInput( kSkyTransmittanceLutBinding, m_TransmittanceLut.get() );
        if ( inFrame )
        {
            renderer.ComputeImageBeginWrite( m_MultiScatterLut.get() );
            renderer.DispatchComputeInFrame( m_MultiScatterLutPipeline.get(),
                                             LutGroupCount( kMultiScatterLutSize ),
                                             LutGroupCount( kMultiScatterLutSize ), 1 );
            renderer.ComputeImageEndWrite( m_MultiScatterLut.get() );
        }
        else
        {
            m_MultiScatterLutPipeline->Dispatch( LutGroupCount( kMultiScatterLutSize ),
                                                 LutGroupCount( kMultiScatterLutSize ), 1 );
        }
    }

    bool SkyboxRenderer::EnsureSkyViewLutResources()
    {
        if ( m_SkyViewResourcesFailed )
            return false;
        if ( m_SkyViewLut )
            return true;

        const Core::Formats::Image2DSpecification skyViewSpec{
             .Tag        = "SkyViewLut",
             .Width      = kSkyViewLutWidth,
             .Height     = kSkyViewLutHeight,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_SkyViewLut = Image2D::Create( skyViewSpec, nullptr );

        if ( !m_SkyViewLut )
        {
            LOG_ERROR( "[SkyAtmosphere] The Sky-View LUT ({}x{} RGBA16F) could not be created; the "
                       "physical sky will not render for this view.",
                       kSkyViewLutWidth, kSkyViewLutHeight );
            m_SkyViewResourcesFailed = true;
            return false;
        }

        LOG_INFO( "[SkyAtmosphere] Sky-View LUT {}x{} RGBA16F ({:.2f} MiB) allocated for the physical "
                  "atmosphere — refilled every frame while the model is active.",
                  kSkyViewLutWidth, kSkyViewLutHeight,
                  LutBytesToMiB( Core::Formats::CalculateImageSize( kSkyViewLutWidth, kSkyViewLutHeight,
                                                                    Core::Formats::ImageFormat::RGBA16F ) ) );
        return true;
    }

    void SkyboxRenderer::DispatchSkyViewLut()
    {
        auto& renderer = Renderer::GetInstance();

        const SkyViewLutPush push{ .CameraPosWorld = glm::vec4( m_ActiveCamera->GetPosition(), 0.0f ) };

        renderer.ComputeImageBeginWrite( m_SkyViewLut.get() );
        m_SkyViewLutPipeline->SetOutput( kSkyViewLutOutputBinding, m_SkyViewLut.get(), 0 );
        m_SkyViewLutPipeline->SetStorageBuffer( kSkyPayloadBinding, m_SkyParams.get() );
        m_SkyViewLutPipeline->SetInput( kSkyTransmittanceLutBinding, m_TransmittanceLut.get() );
        m_SkyViewLutPipeline->SetInput( kSkyMultiScatterLutBinding, m_MultiScatterLut.get() );
        m_SkyViewLutPipeline->SetPushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
        renderer.DispatchComputeInFrame( m_SkyViewLutPipeline.get(), LutGroupCount( kSkyViewLutWidth ),
                                         LutGroupCount( kSkyViewLutHeight ), 1 );
        renderer.ComputeImageEndWrite( m_SkyViewLut.get() );
    }

    bool SkyboxRenderer::EnsureAerialPerspectiveResources()
    {
        if ( m_AerialPerspectiveResourcesFailed )
            return false;
        if ( m_AerialPerspectiveLut )
            return true;

        const Core::Formats::Image3DSpecification apSpec{
             .Tag        = "SkyAerialPerspectiveLut",
             .Width      = kAerialPerspectiveWidth,
             .Height     = kAerialPerspectiveHeight,
             .Depth      = kAerialPerspectiveDepth,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_AerialPerspectiveLut = Image3D::Create( apSpec );

        if ( !m_AerialPerspectiveLut )
        {
            LOG_ERROR( "[SkyAtmosphere] The camera aerial-perspective volume ({}x{}x{} RGBA16F) could not "
                       "be created; geometry in this view will receive no distance haze.",
                       kAerialPerspectiveWidth, kAerialPerspectiveHeight, kAerialPerspectiveDepth );
            m_AerialPerspectiveResourcesFailed = true;
            return false;
        }

        LOG_INFO( "[SkyAtmosphere] Aerial-perspective volume {}x{}x{} RGBA16F ({:.2f} MiB) allocated for "
                  "the physical atmosphere — refilled every frame while the model is active.",
                  kAerialPerspectiveWidth, kAerialPerspectiveHeight, kAerialPerspectiveDepth,
                  LutBytesToMiB( Core::Formats::CalculateImageSize(
                       kAerialPerspectiveWidth, kAerialPerspectiveHeight, kAerialPerspectiveDepth,
                       Core::Formats::ImageFormat::RGBA16F ) ) );
        return true;
    }

    void SkyboxRenderer::DispatchAerialPerspectiveLut()
    {
        auto& renderer = Renderer::GetInstance();

        SkyAerialPerspectivePush push{};
        push.InverseViewProjection =
             glm::inverse( m_ActiveCamera->GetProjectionMatrix() * m_ActiveCamera->GetViewMatrix() );
        push.CameraPosWorld = glm::vec4( m_ActiveCamera->GetPosition(), 0.0f );
        push.VolumeParams =
             glm::vec4( m_Sky.AerialPerspectiveDistanceKm, m_Sky.AerialPerspectiveStartDepthKm, 0.0f, 0.0f );

        renderer.ComputeImageBeginWrite( m_AerialPerspectiveLut.get() );
        m_AerialPerspectivePipeline->SetOutput( kSkyAerialPerspectiveOutputBinding, m_AerialPerspectiveLut.get(),
                                                0 );
        m_AerialPerspectivePipeline->SetStorageBuffer( kSkyPayloadBinding, m_SkyParams.get() );
        m_AerialPerspectivePipeline->SetInput( kSkyTransmittanceLutBinding, m_TransmittanceLut.get() );
        m_AerialPerspectivePipeline->SetInput( kSkyMultiScatterLutBinding, m_MultiScatterLut.get() );
        m_AerialPerspectivePipeline->SetPushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );

        // ONE INVOCATION PER FROXEL COLUMN — the z extent is walked inside the shader so consecutive
        // slices share one quadrature, so the dispatch is 2D over the volume's x/y and never over z.
        renderer.DispatchComputeInFrame( m_AerialPerspectivePipeline.get(),
                                         LutGroupCount( kAerialPerspectiveWidth ),
                                         LutGroupCount( kAerialPerspectiveHeight ), 1 );
        renderer.ComputeImageEndWrite( m_AerialPerspectiveLut.get() );
    }

    bool SkyboxRenderer::EnsureDistantLightResources()
    {
        if ( m_DistantLightResourcesFailed )
            return false;
        if ( m_DistantLight )
            return true;

        // ONE TEXEL: (0,0) the full-sphere mean the height fog reads — one march, one reduction, see
        // Programs/Sky/SkyDistantLight.shader. RGBA32F rather than the RGBA16F every other LUT uses: at
        // 16 bytes total the exact format is free, and this value is added to a fog colour at night
        // radiances where a half's three decimal digits would quantise into visible steps as the sun
        // sets.
        const Core::Formats::Image2DSpecification distantSpec{
             .Tag        = "SkyDistantLight",
             .Width      = kDistantLightWidth,
             .Height     = 1,
             .Format     = Core::Formats::ImageFormat::RGBA32F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_DistantLight = Image2D::Create( distantSpec, nullptr );

        if ( !m_DistantLight )
        {
            LOG_ERROR( "[SkyAtmosphere] The distant sky light ({}x1 RGBA32F) could not be created; the "
                       "atmospheric fog in this view will fall back to its authored colour with no sky "
                       "ambient.",
                       kDistantLightWidth );
            m_DistantLightResourcesFailed = true;
            return false;
        }

        LOG_INFO( "[SkyAtmosphere] Distant sky light {}x1 RGBA32F allocated — {} directions marched at "
                  "{} km every frame, reduced to the full-sphere mean the fog reads.",
                  kDistantLightWidth, 64, 6 );
        return true;
    }

    void SkyboxRenderer::DispatchDistantLight()
    {
        auto& renderer = Renderer::GetInstance();

        renderer.ComputeImageBeginWrite( m_DistantLight.get() );
        m_DistantLightPipeline->SetOutput( kSkyDistantLightOutputBinding, m_DistantLight.get(), 0 );
        m_DistantLightPipeline->SetStorageBuffer( kSkyPayloadBinding, m_SkyParams.get() );
        m_DistantLightPipeline->SetInput( kSkyTransmittanceLutBinding, m_TransmittanceLut.get() );
        m_DistantLightPipeline->SetInput( kSkyMultiScatterLutBinding, m_MultiScatterLut.get() );

        // ONE WORKGROUP, and it must stay one: the 64 directions are reduced in groupshared memory,
        // which no second group can see. The shader's LocalSize is 64 for the same reason.
        renderer.DispatchComputeInFrame( m_DistantLightPipeline.get(), 1, 1, 1 );
        renderer.ComputeImageEndWrite( m_DistantLight.get() );
    }

    void SkyboxRenderer::ExecuteAtmosphereLuts()
    {
        // The gradient model never reaches past this line: no allocation, no dispatch, no fingerprint —
        // an existing scene pays literally nothing for the physical atmosphere's machinery.
        if ( !m_UseProceduralSky || m_Sky.Model != ECS::SkyModel::PhysicalAtmosphere )
            return;

        // A missing shader was already reported by Initialize with the path it expected.
        if ( !m_TransmittanceLutPipeline || !m_MultiScatterLutPipeline || !m_SkyParams )
            return;

        if ( !EnsureAtmosphereLutResources() )
            return;

        const AtmosphereLutFingerprint wanted = LutFingerprintOf( m_Sky );
        if ( !m_LutsValid || wanted != m_LutBaked )
        {
            DESERT_PROFILE_PASS( "Sky: AtmosphereLuts" );

            DispatchCachedAtmosphereLuts( /*inFrame=*/true );

            m_LutBaked  = wanted;
            m_LutsValid = true;

            LOG_INFO( "[SkyAtmosphere] Atmosphere LUTs dispatched (transmittance {}x{}, multi-scattering "
                      "{}x{}) — the atmosphere parameter fingerprint changed.",
                      kTransmittanceLutWidth, kTransmittanceLutHeight, kMultiScatterLutSize,
                      kMultiScatterLutSize );
        }

        // THE TRANSMITTANCE LUT, published for consumers that do not bind the sky parameter buffer.
        //
        // Gated on m_LutsValid and not on the pipeline existing, on the same contract as the aerial
        // perspective volume and the distant sky light below: a non-null handle IS the statement "this
        // texture has been marched", and a consumer that sampled it before the first dispatch would shade
        // its frame with uninitialised device memory. The two radii travel with it because the LUT's
        // Bruneton mapping is a function of the shell and its reader has to reproduce that mapping — the
        // same argument the aerial-perspective volume's two scalars are published under.
        if ( m_LutsValid )
        {
            m_Atmosphere.TransmittanceLut               = m_TransmittanceLut.get();
            m_Atmosphere.TransmittanceLutBottomRadiusKm = AtmosphereBottomRadiusKm( m_Sky );
            m_Atmosphere.TransmittanceLutTopRadiusKm    = AtmosphereTopRadiusKm( m_Sky );
        }

        // The Sky-View LUT, every frame: it depends on the camera's altitude and the sun, which the
        // cached pair deliberately does not. The sky pass samples the previous frame's fill (this slot
        // runs after the graph recorded the Sky pass) — invisible at 192x104 of slowly-varying sky.
        if ( m_SkyViewLutPipeline && m_ActiveCamera && EnsureSkyViewLutResources() )
        {
            DESERT_PROFILE_PASS( "Sky: SkyViewLut" );
            DispatchSkyViewLut();
        }

        // The camera aerial-perspective volume, every frame and for the same reason — it is the camera's
        // own frustum. Published on the AtmosphereEnv only once the fill has actually happened: the
        // handle being non-null is the contract that says "there is aerial perspective this frame", and
        // the atmospheric-fog pass (dispatched immediately after this slot) composes the identity when
        // it is null rather than sampling a volume nobody wrote.
        if ( m_AerialPerspectivePipeline && m_ActiveCamera && EnsureAerialPerspectiveResources() )
        {
            DESERT_PROFILE_PASS( "Sky: AerialPerspectiveLut" );
            DispatchAerialPerspectiveLut();

            m_Atmosphere.AerialPerspectiveVolume            = m_AerialPerspectiveLut.get();
            m_Atmosphere.AerialPerspectiveDepthKm           = m_Sky.AerialPerspectiveDistanceKm;
            m_Atmosphere.AerialPerspectiveViewDistanceScale = m_Sky.AerialPerspectiveViewDistanceScale;
        }

        // The distant sky light, every frame: it is a function of the sun, which moves. Published on
        // the AtmosphereEnv only once the fill has happened, on the same contract as the volume above —
        // a non-null handle IS the statement "there is a physical average sky this frame", and the
        // atmospheric-fog pass keeps its authored colour when it is null rather than sampling a texel
        // nobody wrote.
        if ( m_DistantLightPipeline && EnsureDistantLightResources() )
        {
            DESERT_PROFILE_PASS( "Sky: DistantSkyLight" );
            DispatchDistantLight();

            m_Atmosphere.DistantSkyLight = m_DistantLight.get();
        }
    }

    bool SkyboxRenderer::EnsureCachedLutsForBake()
    {
        if ( !m_TransmittanceLutPipeline || !m_MultiScatterLutPipeline || !m_SkyParams )
            return false;
        if ( !EnsureAtmosphereLutResources() )
            return false;

        const AtmosphereLutFingerprint wanted = LutFingerprintOf( m_Sky );
        if ( m_LutsValid && wanted == m_LutBaked )
            return true;

        // First physical bake of this renderer (or an atmosphere edit in the same frame): the in-frame
        // slot has not run yet, and the bake cannot march empty LUTs. Immediate dispatches fill them
        // now; the caller idles the device around the bake anyway.
        DispatchCachedAtmosphereLuts( /*inFrame=*/false );
        m_LutBaked  = wanted;
        m_LutsValid = true;

        LOG_INFO( "[SkyAtmosphere] Atmosphere LUTs dispatched immediately for the environment bake — "
                  "the bake ran before this frame's in-frame LUT slot." );
        return true;
    }

    void SkyboxRenderer::PrepareCamera( Core::Camera* camera )
    {
        m_ActiveCamera = camera;
    }

    void SkyboxRenderer::PrepareMaterial( const std::shared_ptr<MaterialSkybox>& material, const SkyLook& look )
    {
        // Only record the material here. This can run from the skybox-load command (ExecuteAll) BEFORE
        // BeginScene/PrepareCamera, so the active camera may not exist yet — the camera-dependent bind
        // is deferred to Render(), which always runs with a valid camera.
        //
        // A NULL MATERIAL MEANS "THIS SCENE HAS NO HDR SKYBOX", and it is the producer's way of saying so.
        // It used to return early and change nothing, which made the sentence unsayable: SkyboxECSSystem
        // emitted a command only when a cubemap existed, so deleting the SkyboxComponent — or loading a
        // level that has none onto a renderer that had one — left the previous cubemap drawing behind the
        // new world AND feeding its IBL into every PBR surface. This is the same explicit-absence rule the
        // sky, the fog and the cloud layer already follow, and it is what lets a render system outlive the
        // scene it was built for (IRenderSystem::OnSceneReplaced).
        m_MaterialSkybox = material; // an empty weak_ptr when the scene has none
        // The look is RECORDED, not applied: applying it means a bake, and a bake idles the device.
        // EnsureHdrEnvironment runs it from the pre-graph slot. A scene with no cubemap keeps the
        // identity look so that re-assigning one later is seen as a change rather than as agreement.
        m_SkyboxLook = material ? look : SkyLook{};
    }

    void SkyboxRenderer::EnsureHdrEnvironment( float deltaSeconds )
    {
        const auto material = m_MaterialSkybox.lock();
        if ( !material )
        {
            m_SecondsSinceLookChanged = 0.0f;
            m_SecondsSinceHdrStale    = 0.0f;
            m_LastSeenLook            = SkyLook{};
            return;
        }

        const float dt = glm::max( deltaSeconds, 0.0f );

        // How long the AUTHORED value has held still — the settle half of the gate. Compared against
        // last frame's value rather than against the baked one, because those answer different
        // questions: this one is "has the person stopped dragging", the one below is "is the picture
        // out of date". Conflating them is what made dragging the sun unusable before the sun's own
        // gate existed (SkyRules::SkyEnvironmentRebakeMayRun).
        if ( !( m_SkyboxLook == m_LastSeenLook ) )
        {
            m_LastSeenLook            = m_SkyboxLook;
            m_SecondsSinceLookChanged = 0.0f;
        }
        else
        {
            m_SecondsSinceLookChanged += dt;
        }

        if ( material->BakedLook() == m_SkyboxLook )
        {
            m_SecondsSinceHdrStale = 0.0f;
            return;
        }

        m_SecondsSinceHdrStale += dt;
        if ( !SkyEnvironmentRebakeMayRun( m_SecondsSinceLookChanged, m_SecondsSinceHdrStale,
                                          kSkyRebakeSettleSeconds, kSkyRebakeMaxDeferSeconds ) )
            return;

        const auto started     = std::chrono::steady_clock::now();
        const bool rebaked     = material->EnsureBaked( m_SkyboxLook );
        m_SecondsSinceHdrStale = 0.0f;

        if ( !rebaked )
            return;

        // WALL TIME AROUND THE WHOLE CHAIN, printed rather than assumed — the same line, for the same
        // reason, as the procedural bake's. It is also the only place a thrash between two views
        // sharing one `.hdr` at two different rotations becomes visible.
        const double bakeMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
        LOG_INFO( "[Skybox] HDR environment rebaked in {:.1f} ms for rotation {:.1f} deg, intensity "
                  "{:.2f}, tint ({:.2f}, {:.2f}, {:.2f}). The device is idle for all of it, which is why "
                  "the trigger is the authored value settling and not the frame.",
                  bakeMs, m_SkyboxLook.RotationDegrees, m_SkyboxLook.Intensity, m_SkyboxLook.Tint.x,
                  m_SkyboxLook.Tint.y, m_SkyboxLook.Tint.z );
    }

    void SkyboxRenderer::SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow,
                                           const SkySettings& sky, const SunLightFx& fx )
    {
        m_UseProceduralSky = enabled;
        m_SunDir           = glm::normalize( sunDir );
        m_Sky              = sky;
        m_BakeRequested    = m_BakeRequested || bakeNow;

        // The evaluated sky other renderers consume. It is rebuilt from this frame's numbers rather than
        // accumulated, so a frame in which the sky is switched off publishes Valid == false immediately.
        //
        // The parameter buffer is part of the condition, not an afterthought: a missing ProceduralSky
        // shader leaves the buffer uncreated, and with no buffer the sky is not being evaluated on the GPU
        // at all. Publishing Valid == true then would hand a consumer a sun no pass agrees with.
        if ( enabled && m_SkyParams )
        {
            m_Atmosphere = EvaluateAtmosphere( m_Sky, m_SunDir );

            // UE's PrepareSunLightProxy, scoped to the model by the teamlead's decision (research doc
            // section 5, Q3): in PhysicalAtmosphere the sun light's colour is multiplied by the
            // atmosphere's transmittance toward the sun at ground level; in ArtisticGradient the
            // coupling does not exist, and the documented independence of sky radiance and surface
            // illuminance stands. The per-light opt-out is the second gate.
            //
            // Evaluated HERE rather than by the consumer because this is where the sun and the medium
            // are both in hand, and because it must be one value per frame: two consumers each
            // marching it would be the same quantity computed twice.
            const bool couple =
                 m_Sky.Model == ECS::SkyModel::PhysicalAtmosphere && fx.AffectedByAtmosphereTransmittance;
            m_Atmosphere.SunTransmittanceAtGround =
                 couple ? SunTransmittanceAtGround( m_Sky, m_Atmosphere.SunDirection ) : glm::vec3( 1.0f );

            // The same product SceneRenderer::OnUpdate forms for the light's own colour, published once
            // so the fog's directional lobe reads the sun on the ground instead of re-deriving it from a
            // light list it has no business walking (UE publishes it on the View UB for the same reason).
            m_Atmosphere.SunIlluminanceOnGround = fx.OuterSpaceIlluminance * m_Atmosphere.SunTransmittanceAtGround;

            // THE NUMERATOR OF THAT PRODUCT, published rather than left to be divided back out. A consumer
            // that re-applies the transmittance at its own altitude — the cloud march's per-sample path —
            // needs the pre-atmosphere illuminance, and recovering it as ground / T(ground) is an infinity
            // at a low sun, where T reaches zero channel by channel.
            m_Atmosphere.SunOuterSpaceIlluminance = fx.OuterSpaceIlluminance;

            // A sun below the horizon is a legal authored state (it is night), but it is also what an
            // inverted Translation looks like — and that mistake shipped in four scenes. Say it once, with
            // the number, rather than leaving "why is everything black" to be discovered.
            if ( m_Atmosphere.SunDirection.y < 0.0f && !m_BelowHorizonLogged )
            {
                LOG_WARN( "[SkyAtmosphere] The atmosphere sun is BELOW the horizon (elevation {:.1f} deg) — "
                          "the sky renders as night. If that is not intended, the light's Translation is "
                          "the direction the light TRAVELS, so a sun overhead points DOWN.",
                          glm::degrees( std::asin( glm::clamp( m_Atmosphere.SunDirection.y, -1.0f, 1.0f ) ) ) );
                m_BelowHorizonLogged = true;
            }
        }
        else
        {
            m_Atmosphere = AtmosphereEnv{};
        }

        UploadSkyParams();
    }

    void SkyboxRenderer::UploadSkyParams()
    {
        if ( !m_SkyParams || !m_UseProceduralSky )
            return;

        const SkyGpuPayload payload  = PackSky( m_SunDir, m_Sky );
        const auto          uploaded = m_SkyParams->SetData( &payload, kSkyPayloadBytes );
        if ( !uploaded.IsSuccess() )
            // REPORTED, NOT RETURNED, AND THE REASON IS THE CALL GRAPH. This is called from the settings
            // path — the sky block is re-packed whenever a knob moves — and from the frame path, and
            // neither has anywhere to put a refusal: the caller changed a value, it did not ask for a
            // frame. The consequence is bounded and self-correcting, which is what makes the log the
            // right answer here rather than a shrug: the buffer keeps the previous block, the sky draws
            // one frame behind, and the very next UploadSkyParams overwrites it.
            LOG_ERROR( "[Skybox] the sky block was not uploaded; the sky keeps the previous parameters "
                       "until the next update: {}",
                       uploaded.GetError() );
    }

    void SkyboxRenderer::EnsureProceduralEnvironment( float deltaSeconds )
    {
        if ( !m_UseProceduralSky )
            return;

        const bool explicitRequest = m_BakeRequested;
        m_BakeRequested            = false;

        // How long the sun has held still. Compared by direction rather than by "did anything write it",
        // because the time-of-day driver rewrites the same value every frame when it is paused.
        const float dt = glm::max( deltaSeconds, 0.0f );
        if ( glm::dot( m_LastSeenSunDir - m_SunDir, m_LastSeenSunDir - m_SunDir ) > 1e-12f )
        {
            m_LastSeenSunDir       = m_SunDir;
            m_SecondsSinceSunMoved = 0.0f;
        }
        else
        {
            m_SecondsSinceSunMoved += dt;
        }

        // THIS VIEW'S CLOUD LAYER, asked for BEFORE the trigger rather than after it, because it IS half
        // of the trigger: the panorama carries the clouds now, so a sky whose sun has not moved can still
        // be a different sky. The call resolves the layer's species and guarantees its volumes, which is
        // work the frame is about to do anyway a few lines later — and nothing at all in a scene with no
        // cloud component, which is what every asset thumbnail and mesh preview is.
        const CloudEnvironmentBake clouds = m_SceneRenderer->BuildCloudEnvironmentBake();

        // THE SKY'S OWN INPUTS, as the bake will read them. Formed from the same PackSky the parameter
        // buffer is filled from, so the number cannot describe a sky the dispatch will not see.
        const uint64_t skyFingerprint = SkyBakeFingerprint( PackSky( m_SunDir, m_Sky ),
                                                            static_cast<uint32_t>( m_Sky.EnvironmentResolution ) );

        if ( !ShouldRebakeSkyEnvironment( m_BakedSunDir, m_SunDir, m_Sky.RebakeSunAngleThreshold,
                                          m_Sky.AutoRebakeEnvironment, static_cast<bool>( m_ProceduralEnv ),
                                          explicitRequest, m_BakedCloudFingerprint, clouds.Fingerprint,
                                          m_BakedSkyFingerprint, skyFingerprint ) )
        {
            m_SecondsSinceStale = 0.0f;
            return;
        }

        m_SecondsSinceStale += dt;

        // The Bake button and the very first bake are answers to a question the user just asked, or the
        // difference between an ambient-lit world and an unlit one. Neither waits.
        const bool immediate = explicitRequest || !m_ProceduralEnv;
        if ( !immediate && !SkyEnvironmentRebakeMayRun( m_SecondsSinceSunMoved, m_SecondsSinceStale,
                                                        kSkyRebakeSettleSeconds, kSkyRebakeMaxDeferSeconds ) )
            return;

        m_SecondsSinceStale = 0.0f;

        const SkyEnvironmentSize size = EnvironmentPanoramaSize( m_Sky.EnvironmentResolution );

        if ( m_Sky.EnvironmentResolution == ECS::SkyEnvironmentResolution::High && !m_HighResCostLogged )
        {
            const SkyEnvironmentCost cost = SkyEnvironmentBakeCost( m_Sky.EnvironmentResolution );
            LOG_INFO( "[SkyAtmosphere] Environment bake at High ({}x{}): panorama {:.1f} MiB + "
                      "radiance/irradiance/prefiltered cubes {:.1f} MiB = {:.1f} MiB — paid PER LIVE "
                      "SceneRenderer ({} live now).",
                      size.Width, size.Height, BytesToMiB( cost.PanoramaBytes ), BytesToMiB( cost.CubeBytes ),
                      BytesToMiB( cost.TotalBytes ), SceneRenderer::GetLiveRendererCount() );
            m_HighResCostLogged = true;
        }

        // The bake runs immediate compute dispatches; idle the device first (mirrors the editor's
        // skybox-swap path) since we're recreating GPU images that prior frames may have referenced.
        Renderer::GetInstance().WaitDeviceIdle();

        // The physical model's bake marches the cached LUTs, which the in-frame slot may not have
        // filled yet (the very first frame bakes before it runs). Guarantee them here; if they cannot
        // exist, the bake would produce a black environment and silently look "done" — skip and say so.
        const bool physical = m_Sky.Model == ECS::SkyModel::PhysicalAtmosphere;
        if ( physical && !EnsureCachedLutsForBake() )
        {
            LOG_ERROR( "[SkyAtmosphere] Environment bake skipped: the physical model's atmosphere LUTs "
                       "are unavailable (see the errors above). The previous environment is kept." );
            return;
        }

        // The cloud block goes onto THIS renderer's buffers here — see the members' declaration for why
        // they are not the cloud renderer's own. A layer that is not marched still writes: a storage
        // buffer whose bytes were never set is uninitialised device memory behind a valid descriptor, and
        // the shader's gate is what stops it being read, not the absence of an upload.
        CloudBakeBinding cloudBinding;
        if ( m_CloudBakeParams && m_CloudBakeAuthored )
        {
            const auto params   = m_CloudBakeParams->SetData( &clouds.Params, kCloudPayloadBytes );
            const auto authored = m_CloudBakeAuthored->SetData(
                 &clouds.Authored, static_cast<uint32_t>( sizeof( clouds.Authored ) ) );

            // BOTH, OR NEITHER IS BOUND — and the paragraph above is exactly why. It says a layer that
            // is not marched still WRITES, because uninitialised device memory behind a valid descriptor
            // is what the shader's gate protects against, not an absent upload. An upload that refused
            // leaves the buffer in precisely the state that paragraph forbids, so the bake must run
            // without clouds rather than with a block it cannot vouch for.
            if ( params.IsSuccess() && authored.IsSuccess() )
            {
                cloudBinding.Params   = m_CloudBakeParams.get();
                cloudBinding.Authored = m_CloudBakeAuthored.get();
            }
            else
            {
                LOG_ERROR( "[SkyAtmosphere] the environment bake carries no clouds this time; their "
                           "blocks were not uploaded. params: {} | authored: {}",
                           params.IsSuccess() ? "ok" : params.GetError(),
                           authored.IsSuccess() ? "ok" : authored.GetError() );
            }
        }

        cloudBinding.Marched      = clouds.Marched;
        cloudBinding.SkyOcclusion = clouds.SkyOcclusionValid;
        // WHAT THE BLOCK ABOVE MEANS. The cloud renderer decided it when it packed the block; carrying it
        // is what stops the bake from marching a SunColour that is the sun before the atmosphere and
        // applying no atmosphere to it.
        cloudBinding.PerSampleSunTransmittance = clouds.PerSampleSunTransmittance;
        // The atmosphere's own knob, carried because the bake INTEGRATES the air in front of a cloud
        // instead of fetching the camera aerial-perspective volume the screen march reads — a panorama has
        // no view frustum to froxelize. Without it a scene that pushes the haze out would still see it in
        // its reflections.
        cloudBinding.AerialStartDepthKm = m_Sky.AerialPerspectiveStartDepthKm;
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
            cloudBinding.Noise[slot] = clouds.Noise[slot];
        cloudBinding.Modelling          = clouds.Modelling;
        cloudBinding.AuthoredAtlas      = clouds.AuthoredAtlas;
        cloudBinding.SkyOcclusionVolume = clouds.SkyOcclusionVolume;
        cloudBinding.DistantSkyLight    = m_Atmosphere.DistantSkyLight;

        // THE AUTHORED MEDIUM'S OWN VALUES AND IMAGES. Uploaded onto this renderer's buffer for the reason
        // the two blocks above are, and bound ONLY when there are values: the medium's program declares
        // that block exactly when its schema has something in it, and this backend writes a descriptor for
        // any binding a caller names — including one the layout does not have.
        if ( !clouds.MediumValues.empty() && m_CloudBakeMediumParams )
        {
            const auto uploaded = m_CloudBakeMediumParams->SetData(
                 clouds.MediumValues.data(),
                 static_cast<uint32_t>( clouds.MediumValues.size() * sizeof( glm::vec4 ) ) );
            if ( uploaded.IsSuccess() )
            {
                cloudBinding.MediumParams = m_CloudBakeMediumParams.get();
            }
            else
            {
                // NOT SILENT, AND NOT SUBSTITUTED. An unbound block here means the bake's dispatch is
                // skipped for an invalid descriptor set — the environment then keeps the panorama it had,
                // which is the honest outcome, but only if somebody can find out why.
                LOG_ERROR( "[SkyAtmosphere] the authored medium's {} value(s) were not uploaded for the "
                           "environment bake: {}. The panorama that lights this scene will not be rebuilt.",
                           clouds.MediumValues.size(), uploaded.GetError() );
            }
        }
        cloudBinding.MediumImages = clouds.MediumImages;
        // THE AUTHORED MEDIUM, so the panorama that LIGHTS the scene is compiled under the same
        // substitution the three on-screen cloud programs are. It points at `clouds`, which outlives this
        // call — the bake is a submit-and-wait below.
        cloudBinding.Medium = &clouds.Medium;

        // WALL TIME AROUND THE WHOLE CHAIN, printed rather than assumed. Every dispatch below is the
        // immediate compute path — submit and wait on a fence — so this number is the GPU's, and it is the
        // one that says what a rebake costs the frame it lands in. The cloud march is the only thing that
        // has ever been added to it, so it is also the measurement of this feature.
        const auto bakeStarted = std::chrono::steady_clock::now();

        Environment baked = EnvironmentManager::CreateProcedural(
             size.Width, size.Height, m_SkyParams.get(), physical ? m_TransmittanceLut.get() : nullptr,
             physical ? m_MultiScatterLut.get() : nullptr, cloudBinding );
        if ( !baked )
        {
            // Keep the previous environment and say why; the user can retry with the Bake button. Do NOT
            // stamp m_BakedSunDir — a failed bake must not look like an up-to-date one.
            LOG_ERROR( "[SkyAtmosphere] Environment bake at {}x{} failed — the previous environment is "
                       "kept. The BakeProceduralSky compute shader is the usual cause.",
                       size.Width, size.Height );
            return;
        }

        const Environment previous = m_ProceduralEnv;
        m_ProceduralEnv            = baked;

        // Release the previous baked cubes (the image service owns them until unregistered).
        if ( previous )
        {
            auto* imageService = Runtime::ResourceRegistry::GetImageService();
            imageService->Unregister( previous.RadianceMap );
            imageService->Unregister( previous.IrradianceMap );
            imageService->Unregister( previous.PreFilteredMap );
        }

        m_BakedSunDir           = m_SunDir;
        m_BakedCloudFingerprint = clouds.Fingerprint;
        m_BakedSkyFingerprint   = skyFingerprint;

        const double bakeMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - bakeStarted ).count();

        LOG_INFO( "[SkyAtmosphere] Environment baked at {}x{} in {:.1f} ms — {}. The device is idle for all "
                  "of it, which is why the trigger is the sun, the clouds and the sky's own parameters and "
                  "not the frame.",
                  size.Width, size.Height, bakeMs,
                  clouds.Marched ? ( cloudBinding.SkyOcclusion ? "clouds marched into the panorama, "
                                                                 "sky-occlusion volume read"
                                                               : "clouds marched into the panorama, "
                                                                 "profile-driven ambient occlusion" )
                                 : "sky only (this view has no cloud layer)" );
    }

    void SkyboxRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return;

        builder.AddPass( "SkyboxPass", RenderPhase::Sky, [this]() { Render(); },
                         m_Pipeline ? m_Pipeline->GetSpecification() : GraphicsPipelineSpecification{}, targetFb );
    }

    void SkyboxRenderer::Render()
    {
        auto& renderer = Renderer::GetInstance();

        // Engine-generated procedural atmosphere (no HDR asset needed). The LUTs ride along only once
        // the physical model has allocated them; on the gradient they stay null and the material keeps
        // its fallback descriptors for the two samplers the shader declares.
        if ( m_UseProceduralSky && m_ProceduralPipeline && m_ProceduralMaterial && m_ActiveCamera )
        {
            m_ProceduralMaterial->Update( m_ActiveCamera, m_SkyParams, m_TransmittanceLut.get(),
                                          m_SkyViewLut.get() );
            renderer.SubmitFullscreenQuad( m_ProceduralPipeline.get(),
                                           m_ProceduralMaterial->GetMaterialExecutor() );
            return;
        }

        if ( const auto& material = m_MaterialSkybox.lock() )
        {
            if ( m_ActiveCamera )
                material->BindInputs( { m_ActiveCamera } );
            renderer.SubmitFullscreenQuad( m_Pipeline.get(), material->GetMaterialExecutor() );
        }
    }

} // namespace Desert::Graphic::System

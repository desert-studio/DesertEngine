#include "SkyboxRenderer.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <utility>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RenderConfig.hpp> // GlobalTextureFilterSampler
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SkyGroundTransmittance.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <chrono>
#include <cmath>
#include <format>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SkyboxLutReadShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, TransmittanceLutTexture );
        DESERT_SHADER_PARAMETER( Sampler, GlobalFilterSampler );
        DESERT_SHADER_PARAMETER( Texture, MultiScatterLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SkyViewLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, AerialPerspectiveLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DistantSkyLightTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SkyboxLutWriteShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, TransmittanceLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, MultiScatterLutTexture );
    };
} // namespace

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
        pipeSpec.TargetLayout = SceneTargetLayout();
        pipeSpec.Shader      = m_Shader;

        pipeSpec.CullMode          = CullMode::None;
        pipeSpec.DepthTestEnabled  = false;
        pipeSpec.DepthWriteEnabled = false;

        // Same unchecked GetByName as Tonemap/FXAA: a missing 'Skybox' shader was a null dereference.
        const auto pipeline = Graphic::GraphicsPipeline::Create( pipeSpec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();
        // SkyDepth::BehindScene: the same draw where the scene depth is the far plane (the shader rasterises at
        // depth 0, so CloserOrEqual passes exactly on the cleared texels), never writing it.
        const auto behindSceneSpec = []( Graphic::GraphicsPipelineSpecification spec )
        {
            spec.DebugName         = spec.DebugName + ".BehindScene";
            spec.DepthTestEnabled  = true;
            spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
            spec.DepthWriteEnabled = false;
            return spec;
        };
        const auto behindPipeline = Graphic::GraphicsPipeline::Create( behindSceneSpec( pipeSpec ) );
        if ( !behindPipeline )
            return Common::MakeError( behindPipeline.GetError() );
        m_BehindPipeline = behindPipeline.GetValue();

        // Procedural sky: same fullscreen-triangle pass/target, but the engine-generated atmosphere shader.
        m_ProceduralShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "ProceduralSky" );
        Common::ResultStr<std::shared_ptr<Graphic::GraphicsPipeline>> proceduralPipeline;
        if ( m_ProceduralShader )
        {
            Graphic::GraphicsPipelineSpecification skySpec;
            skySpec.DebugName         = "ProceduralSky";
            skySpec.TargetLayout      = SceneTargetLayout();
            skySpec.Shader            = m_ProceduralShader;
            skySpec.CullMode          = CullMode::None;
            skySpec.DepthTestEnabled  = false;
            skySpec.DepthWriteEnabled = false;

            proceduralPipeline = Graphic::GraphicsPipeline::Create( skySpec );
            if ( proceduralPipeline )
            {
                // Its BehindScene twin is part of the same pipeline: refused, the procedural sky is refused whole
                // (a deferred view would otherwise have no sky draw it could make).
                auto behind = Graphic::GraphicsPipeline::Create( behindSceneSpec( skySpec ) );
                if ( behind )
                    m_ProceduralBehindPipeline = behind.GetValue();
                else
                    proceduralPipeline =
                        Common::MakeError<std::shared_ptr<Graphic::GraphicsPipeline>>( behind.GetError() );
            }
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
            // Persistent and two sets: see the members' declaration (a time-sliced capture reads them over
            // several frames).
            for ( uint32_t set = 0; set < 2u; ++set )
            {
                m_CloudBakeParams[set] = ShaderResources::StorageBuffer::Create(
                     "SkyBakeCloudParams", kCloudPayloadBytes, kSkyBakeCloudParamsBinding, /*persistent=*/true );
                m_CloudBakeAuthored[set] = ShaderResources::StorageBuffer::Create(
                     "SkyBakeCloudAuthored", static_cast<uint32_t>( sizeof( CloudAuthoredPayload ) ),
                     kSkyBakeCloudAuthoredBinding, /*persistent=*/true );
                m_CloudBakeMediumParams[set] = ShaderResources::StorageBuffer::Create(
                     "SkyBakeCloudMediumParams", Core::kCloudMediumParamsBytes, Core::kCloudMediumParamsBinding,
                     /*persistent=*/true );
            }

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

        // The procedural sky is optional (no shader, no pipeline): only a built one has parameters to check.
        if ( m_ProceduralPipeline )
            if ( const auto valid = m_ProceduralLayout.Validate( m_ProceduralPipeline->GetSpecification().Shader,
                                                                 SkyboxProceduralShaderParameters::kAll );
                 !valid )
                return valid;
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
        m_TransmittanceLut = Image2D::Create( transmittanceSpec );

        const Core::Formats::Image2DSpecification multiScatterSpec{
             .Tag        = "SkyMultiScatterLut",
             .Width      = kMultiScatterLutSize,
             .Height     = kMultiScatterLutSize,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_MultiScatterLut = Image2D::Create( multiScatterSpec );

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

    Common::BoolResultStr SkyboxRenderer::DispatchTransmittanceLut( const RDG::PassContext& context )
    {
        // Transmittance strictly first: the multi-scattering march samples it per step (the graph orders the
        // two nodes by their declared write and read). Through the block the node declared
        // (DeclareAtmosphereLutNodes).
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_TransmittanceLutPipeline,
                                                        LutGroupCount( kTransmittanceLutWidth ),
                                                        LutGroupCount( kTransmittanceLutHeight ), 1 );
    }

    Common::BoolResultStr SkyboxRenderer::DispatchMultiScatterLut( const RDG::PassContext& context )
    {
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_MultiScatterLutPipeline,
                                                        LutGroupCount( kMultiScatterLutSize ),
                                                        LutGroupCount( kMultiScatterLutSize ), 1 );
    }

    bool SkyboxRenderer::EnsureSkyViewLutResources()
    {
        if ( m_SkyViewResourcesFailed )
            return false;
        if ( m_SkyViewLut )
            return true;

        // Atmosphere light 1's LUT has the same texel layout, parameterised around that light.
        const auto skyViewSpec = []( const char* tag )
        {
            return Core::Formats::Image2DSpecification{
                 .Tag        = tag,
                 .Width      = kSkyViewLutWidth,
                 .Height     = kSkyViewLutHeight,
                 .Format     = Core::Formats::ImageFormat::RGBA16F,
                 .Mips       = 1u,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
        };
        m_SkyViewLut  = Image2D::Create( skyViewSpec( "SkyViewLut" ) );
        m_SkyViewLut1 = Image2D::Create( skyViewSpec( "SkyViewLut1" ) );

        if ( !m_SkyViewLut || !m_SkyViewLut1 )
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

    Common::BoolResultStr SkyboxRenderer::DispatchSkyViewLut( const RDG::PassContext& context, const int lightIndex )
    {
        auto& renderer = Renderer::GetInstance();

        const SkyViewLutPush push{ .CameraPosWorld =
                                        glm::vec4( m_ActiveCamera->GetPosition(), static_cast<float>( lightIndex ) ) };

        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
        return renderer.DispatchCompute( bindings, *m_SkyViewLutPipeline, LutGroupCount( kSkyViewLutWidth ),
                                         LutGroupCount( kSkyViewLutHeight ), 1 );
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

    Common::BoolResultStr SkyboxRenderer::DispatchAerialPerspectiveLut( const RDG::PassContext& context )
    {
        auto& renderer = Renderer::GetInstance();

        SkyAerialPerspectivePush push{};
        push.InverseViewProjection =
             glm::inverse( m_ActiveCamera->GetProjectionMatrix() * m_ActiveCamera->GetViewMatrix() );
        push.CameraPosWorld = glm::vec4( m_ActiveCamera->GetPosition(), 0.0f );
        push.VolumeParams =
             glm::vec4( m_Sky.AerialPerspectiveDistanceKm, m_Sky.AerialPerspectiveStartDepthKm, 0.0f, 0.0f );

        // ONE INVOCATION PER FROXEL COLUMN — the z extent is walked inside the shader so consecutive
        // slices share one quadrature, so the dispatch is 2D over the volume's x/y and never over z.
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
        return renderer.DispatchCompute( bindings, *m_AerialPerspectivePipeline,
                                         LutGroupCount( kAerialPerspectiveWidth ),
                                         LutGroupCount( kAerialPerspectiveHeight ), 1 );
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
        m_DistantLight = Image2D::Create( distantSpec );

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

    Common::BoolResultStr SkyboxRenderer::DispatchDistantLight( const RDG::PassContext& context )
    {
        auto& renderer = Renderer::GetInstance();

        // ONE WORKGROUP, and it must stay one: the 64 directions are reduced in groupshared memory,
        // which no second group can see. The shader's LocalSize is 64 for the same reason.
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return renderer.DispatchCompute( bindings, *m_DistantLightPipeline, 1, 1, 1 );
    }

    RenderPassDeclaration::BlockDeclaration SkyboxRenderer::DeclareLutBlock( RenderPassDeclaration&    declared,
                                                                             ComputePipeline*          pipeline,
                                                                             ShaderBindingLayoutCache& layout,
                                                                             const uint32_t            pushBytes )
    {
        // Each node's one binding block (block 0): the pipeline's shader layout and the push block its exec
        // gives. Every LUT is a block entry naming the renderer's own image (imported by the graph, which orders,
        // barriers and fault-isolates it; written as a storage image, sampled with the sampler the image carries
        // as its own: GlobalTextureFilterSampler); the pipeline route carries only the sky payload buffer, set
        // here in setup every frame.
        pipeline->SetStorageBuffer( kSkyPayloadBinding, m_SkyParams.get() );
        auto block = declared.Bindings( layout.Get( pipeline->GetSpecification().Shader ),
                                        Renderer::GetPipelineRouteFill( *pipeline ) );
        block.PushConstantBytes( pushBytes );
        return block;
    }

    std::vector<ComputeNodeDeclaration> SkyboxRenderer::DeclareAtmosphereCachedLutNodes()
    {
        // One Compute node per dispatch, each declaring the LUT it writes (StorageWrite) and the LUTs it samples
        // (SampledCompute): the graph orders transmittance -> multi-scattering -> distant light and places every
        // barrier. What runs is decided here, while the frame graph is built.
        std::vector<ComputeNodeDeclaration> nodes;
        m_LutBakePending.reset();
        if ( !m_UseProceduralSky || m_Sky.Model != ECS::SkyModel::PhysicalAtmosphere )
            return nodes;
        if ( !m_TransmittanceLutPipeline || !m_MultiScatterLutPipeline || !m_SkyParams )
            return nodes;
        if ( !EnsureAtmosphereLutResources() )
            return nodes;

        const auto declareBlock = [this]( RenderPassDeclaration& declared, ComputePipeline* pipeline,
                                          ShaderBindingLayoutCache& layout, const uint32_t pushBytes )
        { return DeclareLutBlock( declared, pipeline, layout, pushBytes ); };
        const auto sampledLuts = [this]( RenderPassDeclaration::BlockDeclaration block )
        {
            block.Texture( SkyboxLutReadShaderParameters::TransmittanceLutTexture, m_TransmittanceLut,
                           RDG::Access::SampledCompute, "Sky.TransmittanceLut" )
                 .Sampler( SkyboxLutReadShaderParameters::GlobalFilterSampler, GlobalTextureFilterSampler() )
                 .Texture( SkyboxLutReadShaderParameters::MultiScatterLutTexture, m_MultiScatterLut,
                           RDG::Access::SampledCompute, "Sky.MultiScatterLut" );
            return block;
        };

        // The cached pair only when the atmosphere's parameters changed.
        const AtmosphereLutFingerprint wanted = LutFingerprintOf( m_Sky );
        if ( !m_LutsValid || wanted != m_LutBaked )
        {
            ComputeNodeDeclaration transmittance;
            transmittance.Name = "Sky: TransmittanceLut";
            declareBlock( transmittance.Access, m_TransmittanceLutPipeline.get(), m_TransmittanceLutLayout, 0 )
                 .Storage( SkyboxLutWriteShaderParameters::TransmittanceLutTexture, m_TransmittanceLut,
                           RDG::Access::StorageWrite, "Sky.TransmittanceLut" );
            transmittance.Record = [this]( RDG::PassContext& context,
                                           const FrameGraphRefs& ) -> Common::BoolResultStr
            {
                DESERT_PROFILE_PASS( "Sky: AtmosphereLuts" );
                return DispatchTransmittanceLut( context );
            };
            nodes.push_back( std::move( transmittance ) );

            ComputeNodeDeclaration multiScatter;
            multiScatter.Name = "Sky: MultiScatterLut";
            declareBlock( multiScatter.Access, m_MultiScatterLutPipeline.get(), m_MultiScatterLutLayout, 0 )
                 .Texture( SkyboxLutReadShaderParameters::TransmittanceLutTexture, m_TransmittanceLut,
                           RDG::Access::SampledCompute, "Sky.TransmittanceLut" )
                 .Sampler( SkyboxLutReadShaderParameters::GlobalFilterSampler, GlobalTextureFilterSampler() )
                 .Storage( SkyboxLutWriteShaderParameters::MultiScatterLutTexture, m_MultiScatterLut,
                           RDG::Access::StorageWrite, "Sky.MultiScatterLut" );
            multiScatter.Record = [this]( RDG::PassContext& context,
                                          const FrameGraphRefs& ) -> Common::BoolResultStr
            { return DispatchMultiScatterLut( context ); };
            nodes.push_back( std::move( multiScatter ) );

            // BUILD TIME: which nodes exist is decided here; the pair counts as baked once
            // SettleAtmosphereCachedLutNodes hears the graph accepted them.
            m_LutBakePending = wanted;
        }

        // BUILD TIME: the LUT handles feed the atmosphere the later nodes of this graph declare and bind.
        if ( m_LutsValid || m_LutBakePending )
        {
            m_Atmosphere.TransmittanceLut               = m_TransmittanceLut;
            m_Atmosphere.TransmittanceLutBottomRadiusKm = AtmosphereBottomRadiusKm( m_Sky );
            m_Atmosphere.TransmittanceLutTopRadiusKm    = AtmosphereTopRadiusKm( m_Sky );
        }

        if ( m_DistantLightPipeline && EnsureDistantLightResources() )
        {
            ComputeNodeDeclaration distant;
            distant.Name = "Sky: DistantSkyLight";
            sampledLuts( declareBlock( distant.Access, m_DistantLightPipeline.get(), m_DistantLightLayout, 0 ) )
                 .Storage( SkyboxLutReadShaderParameters::DistantSkyLightTexture, m_DistantLight,
                           RDG::Access::StorageWrite, "Sky.DistantLight" );
            distant.Record = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
            {
                DESERT_PROFILE_PASS( "Sky: DistantSkyLight" );
                return DispatchDistantLight( context );
            };
            nodes.push_back( std::move( distant ) );

            m_Atmosphere.DistantSkyLight = m_DistantLight;
        }
        return nodes;
    }

    void SkyboxRenderer::SettleAtmosphereCachedLutNodes( bool accepted )
    {
        const std::optional<AtmosphereLutFingerprint> pending = std::exchange( m_LutBakePending, std::nullopt );
        if ( !accepted )
        {
            // Nothing was recorded: a pair that was about to be baked is not baked, so the next frame bakes it.
            if ( pending )
                m_LutsValid = false;
            return;
        }
        if ( !pending )
            return;
        m_LutBaked  = *pending;
        m_LutsValid = true;
        LOG_INFO( "[SkyAtmosphere] Atmosphere LUTs dispatched (transmittance {}x{}, multi-scattering {}x{}) — the "
                  "atmosphere parameter fingerprint changed.",
                  kTransmittanceLutWidth, kTransmittanceLutHeight, kMultiScatterLutSize, kMultiScatterLutSize );
    }

    std::vector<ComputeNodeDeclaration> SkyboxRenderer::DeclareAtmosphereLutNodes()
    {
        // The per-view LUTs, the Sky-View LUT and the aerial-perspective volume, both following the camera. They
        // sample the cached pair DeclareAtmosphereCachedLutNodes added at the head of this graph.
        std::vector<ComputeNodeDeclaration> nodes;
        m_SkyViewLutFillPending  = false;
        m_SkyViewLut1FillPending = false;
        if ( !m_UseProceduralSky || m_Sky.Model != ECS::SkyModel::PhysicalAtmosphere )
            return nodes;
        if ( !m_SkyParams || !m_TransmittanceLut || !m_MultiScatterLut || !m_LutsValid )
            return nodes;

        const auto declareBlock = [this]( RenderPassDeclaration& declared, ComputePipeline* pipeline,
                                          ShaderBindingLayoutCache& layout, const uint32_t pushBytes )
        { return DeclareLutBlock( declared, pipeline, layout, pushBytes ); };
        const auto sampledLuts = [this]( RenderPassDeclaration::BlockDeclaration block )
        {
            block.Texture( SkyboxLutReadShaderParameters::TransmittanceLutTexture, m_TransmittanceLut,
                           RDG::Access::SampledCompute, "Sky.TransmittanceLut" )
                 .Sampler( SkyboxLutReadShaderParameters::GlobalFilterSampler, GlobalTextureFilterSampler() )
                 .Texture( SkyboxLutReadShaderParameters::MultiScatterLutTexture, m_MultiScatterLut,
                           RDG::Access::SampledCompute, "Sky.MultiScatterLut" );
            return block;
        };

        if ( m_SkyViewLutPipeline && m_ActiveCamera && EnsureSkyViewLutResources() )
        {
            ComputeNodeDeclaration skyView;
            skyView.Name = "Sky: SkyViewLut";
            sampledLuts( declareBlock( skyView.Access, m_SkyViewLutPipeline.get(), m_SkyViewLutLayout,
                                       static_cast<uint32_t>( sizeof( SkyViewLutPush ) ) ) )
                 .Storage( SkyboxLutReadShaderParameters::SkyViewLutTexture, m_SkyViewLut,
                           RDG::Access::StorageWrite, "Sky.SkyViewLut" );
            skyView.Record = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
            {
                DESERT_PROFILE_PASS( "Sky: SkyViewLut" );
                return DispatchSkyViewLut( context, 0 );
            };
            nodes.push_back( std::move( skyView ) );
            // BUILD TIME: the fill counts once SettleAtmosphereLutNodes hears the graph accepted the node.
            m_SkyViewLutFillPending = true;

            // Atmosphere light 1 (the moon): its own fill, only while the scene has one.
            if ( m_Light1.Present )
            {
                ComputeNodeDeclaration skyView1;
                skyView1.Name = "Sky: SkyViewLut1";
                sampledLuts( declareBlock( skyView1.Access, m_SkyViewLutPipeline.get(), m_SkyViewLutLayout,
                                           static_cast<uint32_t>( sizeof( SkyViewLutPush ) ) ) )
                     .Storage( SkyboxLutReadShaderParameters::SkyViewLutTexture, m_SkyViewLut1,
                               RDG::Access::StorageWrite, "Sky.SkyViewLut1" );
                skyView1.Record = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
                {
                    DESERT_PROFILE_PASS( "Sky: SkyViewLut1" );
                    return DispatchSkyViewLut( context, 1 );
                };
                nodes.push_back( std::move( skyView1 ) );
                m_SkyViewLut1FillPending = true;
            }
        }

        if ( m_AerialPerspectivePipeline && m_ActiveCamera && EnsureAerialPerspectiveResources() )
        {
            // A volume (Image3D), imported through the one ImportImage path.
            ComputeNodeDeclaration aerial;
            aerial.Name = "Sky: AerialPerspectiveLut";
            sampledLuts( declareBlock( aerial.Access, m_AerialPerspectivePipeline.get(), m_AerialPerspectiveLayout,
                                       static_cast<uint32_t>( sizeof( SkyAerialPerspectivePush ) ) ) )
                 .Storage( SkyboxLutReadShaderParameters::AerialPerspectiveLutTexture, m_AerialPerspectiveLut,
                           RDG::Access::StorageWrite, "Sky.AerialPerspectiveLut" );
            aerial.Record = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
            {
                DESERT_PROFILE_PASS( "Sky: AerialPerspectiveLut" );
                return DispatchAerialPerspectiveLut( context );
            };
            nodes.push_back( std::move( aerial ) );

            m_Atmosphere.AerialPerspectiveVolume            = m_AerialPerspectiveLut;
            m_Atmosphere.AerialPerspectiveDepthKm           = m_Sky.AerialPerspectiveDistanceKm;
            m_Atmosphere.AerialPerspectiveViewDistanceScale = m_Sky.AerialPerspectiveViewDistanceScale;
        }

        return nodes;
    }

    void SkyboxRenderer::SettleAtmosphereLutNodes( bool accepted )
    {
        // A refused sky-view node wrote nothing; a LUT an earlier accepted frame filled keeps that content.
        if ( std::exchange( m_SkyViewLutFillPending, false ) && accepted )
            m_SkyViewLutFilled = true;
        if ( std::exchange( m_SkyViewLut1FillPending, false ) && accepted )
            m_SkyViewLut1Filled = true;
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
        // new world AND feeding its IBL into every lit surface. This is the same explicit-absence rule the
        // sky, the fog and the cloud layer already follow, and it is what lets a render system outlive the
        // scene it was built for (IRenderSystem::OnSceneReplaced).
        m_MaterialSkybox = material; // an empty weak_ptr when the scene has none
        // A scene with no cubemap holds the identity look, so nothing stale rides along with the next one.
        m_SkyboxLook = material ? look : SkyLook{};
    }

    void SkyboxRenderer::SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow,
                                           const SkySettings& sky, const SunLightFx& fx,
                                           const AtmosphereLight1& light1 )
    {
        m_UseProceduralSky = enabled;
        m_SunDir           = glm::normalize( sunDir );
        m_Light1           = light1;
        m_Sky              = sky;
        m_BakeRequested    = m_BakeRequested || bakeNow;

        // The physical sky's sun is the atmosphere sun LIGHT (UE): no light chosen, no sun in the sky.
        m_SunInputs.OuterSpaceIlluminance = fx.OuterSpaceIlluminance;
        m_SunInputs.DiskColorScale        = fx.SunDiskColorScale;

        // The evaluated sky other renderers consume. It is rebuilt from this frame's numbers rather than
        // accumulated, so a frame in which the sky is switched off publishes Valid == false immediately.
        //
        // The parameter buffer is part of the condition, not an afterthought: a missing ProceduralSky
        // shader leaves the buffer uncreated, and with no buffer the sky is not being evaluated on the GPU
        // at all. Publishing Valid == true then would hand a consumer a sun no pass agrees with.
        if ( enabled && m_SkyParams )
        {
            m_Atmosphere = EvaluateAtmosphere( m_Sky, m_SunDir );

            // UE's PrepareSunLightProxy, plus the planet's shadow, decided in ONE place for both sky
            // models (Graphic::SunLightFactorAtGround says which term applies when): a sun below the
            // horizon gives no direct light in either model; above it both dim and redden it by the
            // atmosphere's transmittance (the LUT's march, or the same medium by analytic air mass).
            //
            // Evaluated HERE rather than by the consumer because this is where the sun and the medium
            // are both in hand, and because it must be one value per frame: two consumers each
            // marching it would be the same quantity computed twice.
            m_Atmosphere.SunTransmittanceAtGround =
                 SunLightFactorAtGround( m_Sky, m_Atmosphere.SunDirection, fx.AffectedByAtmosphereTransmittance );
            // Atmosphere light 1 by the same rule through its own direction (1 when there is none: nothing reads it).
            m_Atmosphere.Light1TransmittanceAtGround =
                 m_Light1.Present
                      ? SunLightFactorAtGround( m_Sky, m_Light1.TowardLight, m_Light1.AffectedByAtmosphereTransmittance )
                      : glm::vec3( 1.0f );

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
        // NOT uploaded here (SKY-BACKDROP): this runs from the frame's command replay, which Scene executes
        // BETWEEN the view's BeginScene and OnUpdate — outside any ActiveViewScope. The non-persistent block
        // then went to the frame context's copy while the pass bound the view's, so the sky drew the block of
        // the one upload that happened inside a scope (the first exposure change) and never moved again: the
        // sun froze at its load-time elevation and a second scene drew the first one's palette. The block is
        // the view's frame state (UE: the sky's view uniform buffer) and is written by UploadFrameSkyParams,
        // which SceneRenderer::OnUpdate calls inside its scope every frame.
    }

    void SkyboxRenderer::SetSkyLuminanceCeiling( float ceiling )
    {
        // State only; the block carrying it is written once per frame by UploadFrameSkyParams.
        m_SunInputs.LuminanceCeiling = ceiling;
    }

    void SkyboxRenderer::UploadFrameSkyParams()
    {
        if ( !m_SkyParams || !m_UseProceduralSky )
            return;

        const SkyGpuPayload payload  = PackSky( m_SunDir, m_Sky, m_SunInputs, m_Light1 );
        const auto          uploaded = m_SkyParams->SetData( &payload, kSkyPayloadBytes );
        if ( !uploaded.IsSuccess() )
            // REPORTED, NOT RETURNED: SceneRenderer::OnUpdate has nowhere to put a refusal of one block.
            // The consequence is bounded and self-correcting: the buffer keeps the previous block, the sky
            // draws one frame behind, and the next frame's UploadFrameSkyParams overwrites it.
            LOG_ERROR( "[Skybox] the sky block was not uploaded; the sky keeps the previous parameters "
                       "until the next update: {}",
                       uploaded.GetError() );
    }

    void SkyboxRenderer::EnsureProceduralEnvironment( const float deltaSeconds )
    {
        // A view lit by a lent environment (BorrowEnvironment) bakes none.
        if ( !m_UseProceduralSky || m_BorrowsEnvironment )
            return;

        // THE SCENE'S SKY LIGHT. Every view rendering a scene is handed it by BeginScene, so a view without one
        // is a renderer driven outside a scene -- said once, and the view has no sky light.
        const std::shared_ptr<SceneSkyLight> skyLight = m_SceneSkyLight.lock();
        if ( !skyLight )
        {
            if ( !m_NoSkyLightLogged )
                LOG_ERROR( "[SkyAtmosphere] this view renders a procedural sky without a scene's sky light "
                           "(SceneRenderer::BeginScene hands it in); the view has no sky ambient." );
            m_NoSkyLightLogged = true;
            return;
        }
        // The Bake button's request is the scene's, whichever view's component asked.
        if ( m_BakeRequested )
            skyLight->RequestCapture();
        m_BakeRequested = false;

        // ONE CAPTURE PER SCENE: the first view of the frame decides (and records, AddSkyCapturePasses); the views
        // after it read what it left.
        const float dt = glm::max( deltaSeconds, 0.0f );
        if ( !skyLight->ClaimFrame( this, m_SunDir, dt ) )
            return;

        // A CAPTURE STILL BEING RECORDED is answered before anything else: its slices run on the frames after
        // it began, and until its last one no second capture starts -- a request that arrives meanwhile stays
        // with the scene's sky light and is read once this one has completed.
        if ( skyLight->IsCapturing() )
            return;
        if ( !skyLight->EnsurePrograms() )
            return;

        const bool explicitRequest = skyLight->TakeCaptureRequest();
        const bool hasEnvironment  = skyLight->HasEnvironment();

        // THIS VIEW'S CLOUD LAYER, asked for BEFORE the trigger rather than after it, because it IS half
        // of the trigger: the capture carries the clouds now, so a sky whose sun has not moved can still
        // be a different sky. The call resolves the layer's species and guarantees its volumes, which is
        // work the frame is about to do anyway a few lines later — and nothing at all in a scene with no
        // cloud component, which is what every asset thumbnail and mesh preview is.
        const CloudEnvironmentBake clouds = m_SceneRenderer->BuildCloudEnvironmentBake();
        // ...but NEVER THE FIRST CAPTURE: a view without one has no sky light at all, and an ambient lit by the
        // sky before its clouds arrive is far nearer the answer than a black one. The clouds landing changes the
        // cloud fingerprint, which recaptures with them -- whole, in that one frame, being the first capture
        // that carries them (SkyCaptureIsWhole).
        if ( hasEnvironment && SkyEnvironmentBakeWaitsForClouds( explicitRequest, clouds.InputsPending ) )
            return;

        // THE SKY'S OWN INPUTS, as the bake will read them. Formed from the same PackSky the parameter
        // buffer is filled from, so the number cannot describe a sky the dispatch will not see.
        const uint64_t skyFingerprint = SkyBakeFingerprint( PackSky( m_SunDir, m_Sky, m_SunInputs, m_Light1 ),
                                                            static_cast<uint32_t>( m_Sky.EnvironmentResolution ) );

        const SkyLightStamps& captured = skyLight->GetCaptured();
        if ( !ShouldRebakeSkyEnvironment( captured.SunDir, m_SunDir, m_Sky.RebakeSunAngleThreshold,
                                          m_Sky.AutoRebakeEnvironment, hasEnvironment, explicitRequest,
                                          captured.CloudFingerprint, clouds.Fingerprint, captured.SkyFingerprint,
                                          skyFingerprint ) )
        {
            skyLight->ClearStale();
            return;
        }

        const float secondsSinceStale = skyLight->AccumulateStaleSeconds( dt );

        // The Bake button and the very first bake are answers to a question the user just asked, or the
        // difference between an ambient-lit world and an unlit one. Neither waits.
        const bool immediate = explicitRequest || !hasEnvironment;
        if ( !immediate && !SkyEnvironmentRebakeMayRun( skyLight->GetSecondsSinceSunMoved(), secondsSinceStale,
                                                        kSkyRebakeSettleSeconds, kSkyRebakeMaxDeferSeconds ) )
            return;

        // The physical model's capture marches the cached LUTs, which this frame's graph writes ahead of it
        // (DeclareAtmosphereCachedLutNodes): no frame of waiting for them. A LUT pipeline that never came up is
        // the one case that stops the capture, said once.
        const bool physical = m_Sky.Model == ECS::SkyModel::PhysicalAtmosphere;
        if ( physical && ( !m_TransmittanceLutPipeline || !m_MultiScatterLutPipeline || m_LutResourcesFailed ) )
        {
            if ( !m_CaptureLutsMissingLogged )
                LOG_ERROR(
                     "[SkyAtmosphere] the sky light is not captured: the physical model's atmosphere LUTs are "
                     "unavailable (see the errors above). The previous environment, if any, is kept." );
            m_CaptureLutsMissingLogged = true;
            return;
        }

        skyLight->ClearStale();

        const SkyEnvironmentSize size = EnvironmentCaptureSize( m_Sky.EnvironmentResolution );

        if ( m_Sky.EnvironmentResolution == ECS::SkyEnvironmentResolution::High && !m_HighResCostLogged )
        {
            const SkyEnvironmentCost cost = SkyEnvironmentBakeCost( m_Sky.EnvironmentResolution );
            LOG_INFO( "[SkyAtmosphere] Sky light captured at High ({} face): radiance cube {:.1f} MiB while it "
                      "runs + irradiance/prefiltered cubes {:.1f} MiB = {:.1f} MiB, and the clouds are marched "
                      "over four times Medium's texels on every recapture -- paid once per scene, whatever its "
                      "views.",
                      size.Face, BytesToMiB( cost.RadianceBytes ), BytesToMiB( cost.CubeBytes ),
                      BytesToMiB( cost.TotalBytes ) );
            m_HighResCostLogged = true;
        }

        // NO DEVICE IDLE. The capture only CREATES images here; the environment it replaces is released when the
        // new one completes, through the image service, whose destruction is deferred past the frames in flight
        // (VulkanAllocator::RT_DestroyImage) -- the same path every other released image takes.

        // The cloud block goes onto THIS renderer's buffers here — see the members' declaration for why
        // they are not the cloud renderer's own. A layer that is not marched still writes: a storage
        // buffer whose bytes were never set is uninitialised device memory behind a valid descriptor, and
        // the shader's gate is what stops it being read, not the absence of an upload.
        CloudBakeBinding cloudBinding;
        m_CloudBakeSet                                                      = 1u - m_CloudBakeSet;
        const std::shared_ptr<ShaderResources::StorageBuffer>& bakeParams   = m_CloudBakeParams[m_CloudBakeSet];
        const std::shared_ptr<ShaderResources::StorageBuffer>& bakeAuthored = m_CloudBakeAuthored[m_CloudBakeSet];
        const std::shared_ptr<ShaderResources::StorageBuffer>& bakeMedium =
             m_CloudBakeMediumParams[m_CloudBakeSet];
        if ( bakeParams && bakeAuthored )
        {
            const auto params = bakeParams->SetData( &clouds.Params, kCloudPayloadBytes );
            const auto authored =
                 bakeAuthored->SetData( &clouds.Authored, static_cast<uint32_t>( sizeof( clouds.Authored ) ) );

            // BOTH, OR NEITHER IS BOUND — and the paragraph above is exactly why. It says a layer that
            // is not marched still WRITES, because uninitialised device memory behind a valid descriptor
            // is what the shader's gate protects against, not an absent upload. An upload that refused
            // leaves the buffer in precisely the state that paragraph forbids, so the bake must run
            // without clouds rather than with a block it cannot vouch for.
            if ( params.IsSuccess() && authored.IsSuccess() )
            {
                cloudBinding.Params   = bakeParams;
                cloudBinding.Authored = bakeAuthored;
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
        // instead of fetching the camera aerial-perspective volume the screen march reads — that volume is the
        // camera's froxels, and the capture is anchored at a fixed point, not at the camera. Without it a scene
        // that pushes the haze out would still see it in its reflections.
        cloudBinding.AerialStartDepthKm = m_Sky.AerialPerspectiveStartDepthKm;
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
            cloudBinding.Noise[slot] = clouds.Noise[slot];
        cloudBinding.Modelling          = clouds.Modelling;
        cloudBinding.FarWeather         = clouds.FarWeather;
        cloudBinding.LayoutPattern      = clouds.LayoutPattern;
        cloudBinding.LayoutMask         = clouds.LayoutMask;
        cloudBinding.AuthoredAtlas      = clouds.AuthoredAtlas;
        cloudBinding.SkyOcclusionVolume = clouds.SkyOcclusionVolume;

        // THE AUTHORED MEDIUM'S OWN VALUES AND IMAGES. Uploaded onto this renderer's buffer for the reason
        // the two blocks above are, and bound ONLY when there are values: the medium's program declares
        // that block exactly when its schema has something in it, and this backend writes a descriptor for
        // any binding a caller names — including one the layout does not have.
        if ( !clouds.MediumValues.empty() && bakeMedium )
        {
            const auto uploaded =
                 bakeMedium->SetData( clouds.MediumValues.data(),
                                      static_cast<uint32_t>( clouds.MediumValues.size() * sizeof( glm::vec4 ) ) );
            if ( uploaded.IsSuccess() )
            {
                cloudBinding.MediumParams = bakeMedium;
            }
            else
            {
                // NOT SILENT, AND NOT SUBSTITUTED. An unbound block here means the bake's dispatch is
                // skipped for an invalid descriptor set — the environment then keeps the sky light it had,
                // which is the honest outcome, but only if somebody can find out why.
                LOG_ERROR( "[SkyAtmosphere] the authored medium's {} value(s) were not uploaded for the "
                           "environment bake: {}. The sky light of this scene will not be recaptured.",
                           clouds.MediumValues.size(), uploaded.GetError() );
            }
        }
        cloudBinding.MediumImages = clouds.MediumImages;
        // THE AUTHORED MEDIUM, so the capture that LIGHTS the scene is compiled under the same
        // substitution the three on-screen cloud programs are. It points at `clouds`, which outlives Begin --
        // the variant program is acquired there, and the capture keeps the program, not this pointer.
        cloudBinding.Medium = &clouds.Medium;
        // The distant sky light is not part of the binding: this frame's graph writes it ahead of the faces,
        // which declares it as a read (AddSkyCapturePasses).

        const char* what =
             !clouds.Marched ? ( clouds.InputsPending ? "sky only (the cloud layer's inputs are still arriving)"
                                                      : "sky only (this view has no cloud layer)" )
             : cloudBinding.SkyOcclusion ? "clouds marched into the cube faces, sky-occlusion volume read"
                                         : "clouds marched into the cube faces, profile-driven ambient "
                                           "occlusion";
        // WHICH KEY MOVED, for a recapture: the landing line says what was captured, this says why.
        if ( hasEnvironment )
            LOG_INFO( "[SkyAtmosphere] Sky light recapture begins: {}{}{}{}.",
                      explicitRequest ? "requested; " : "",
                      captured.CloudFingerprint != clouds.Fingerprint ? "the clouds changed; " : "",
                      captured.SkyFingerprint != skyFingerprint ? "the sky's parameters changed; " : "",
                      std::format( "the sun is {:.2f} deg from the captured one",
                                   glm::degrees( std::acos( glm::clamp(
                                        glm::dot( glm::normalize( captured.SunDir ), glm::normalize( m_SunDir ) ),
                                        -1.0f, 1.0f ) ) ) ) );
        // A refusal keeps the previous environment and says why (Begin logged it); the user can retry with the
        // Bake button. Nothing is stamped -- a failed capture must not look like an up-to-date one.
        (void)skyLight->Begin( size,
                               System::SkyCaptureSource{
                                    .SkyParams = m_SkyParams.get(), .Clouds = cloudBinding, .Physical = physical },
                               SkyLightStamps{ .SunDir           = m_SunDir,
                                               .CloudFingerprint = clouds.Fingerprint,
                                               .SkyFingerprint   = skyFingerprint,
                                               .CloudsMarched    = cloudBinding.Marched && cloudBinding.Params &&
                                                                cloudBinding.Authored },
                               what );
    }

    void SkyboxRenderer::AddSkyCapturePasses( RDG::Builder& graph, FrameTextures& textures )
    {
        const std::shared_ptr<SceneSkyLight> skyLight = m_SceneSkyLight.lock();
        if ( !m_UseProceduralSky || !skyLight )
            return;

        // WHAT THE CAPTURED SKY LIGHT DELIVERS, against the sun, once per capture: the horizontal illuminance of
        // the sky (the irradiance cube at the zenith) and of the sun on the ground (its illuminance through the
        // atmosphere times the cosine of its zenith angle). A clear sky under a high sun gives roughly 10-20 % of
        // the sun's -- the ratio a shadow on open ground reads at, since that shadow is lit by the sky alone.
        if ( const std::optional<glm::vec3> sky = skyLight->ReadBackZenithIlluminance( this ) )
        {
            const auto luminance = []( const glm::vec3& c )
            { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };
            const float skyLux = luminance( *sky );
            const float sunLux =
                 luminance( m_Atmosphere.SunIlluminanceOnGround ) * glm::max( m_Atmosphere.SunDirection.y, 0.0f );
            LOG_INFO( "[SkyAtmosphere] Sky light illuminance on a horizontal surface: sky {:.0f} lx (rgb {:.0f}, "
                      "{:.0f}, {:.0f}), sun {:.0f} lx at elevation {:.1f} deg; shade/sun {:.1f} %, sky/(sun+sky) "
                      "{:.1f} %.",
                      skyLux, sky->r, sky->g, sky->b, sunLux,
                      glm::degrees( std::asin( glm::clamp( m_Atmosphere.SunDirection.y, -1.0f, 1.0f ) ) ),
                      sunLux > 0.0f ? 100.0f * skyLux / sunLux : 0.0f,
                      skyLux + sunLux > 0.0f ? 100.0f * skyLux / ( skyLux + sunLux ) : 0.0f );
        }
        if ( !skyLight->IsCapturing() )
            return;

        // The physical march reads the cached pair this frame's graph has written (or an earlier one has, when
        // it was current): never a pair that is neither. The capture waits, said by the LUT errors themselves.
        const bool physical = m_Sky.Model == ECS::SkyModel::PhysicalAtmosphere;
        if ( physical && !m_LutsValid )
            return;

        // Recorded only when this view claimed the frame (SceneSkyLight::AddPasses): the LUTs it reads are this
        // view's, written ahead of it in this graph.
        skyLight->AddPasses(
             this, graph, textures,
             System::SkyCaptureFrameReads{ .SkyParams        = m_SkyParams.get(),
                                           .TransmittanceLut = physical ? m_TransmittanceLut : nullptr,
                                           .MultiScatterLut  = physical ? m_MultiScatterLut : nullptr,
                                           .DistantSkyLight  = m_Atmosphere.DistantSkyLight } );
    }

    SystemRasterPass SkyboxRenderer::SkyPass()
    {
        return SkyPassOf( SkyDepth::Fill );
    }

    SystemRasterPass SkyboxRenderer::SkyBehindScenePass()
    {
        return SkyPassOf( SkyDepth::BehindScene );
    }

    SystemRasterPass SkyboxRenderer::SkyPassOf( const SkyDepth depth )
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return {};

        SystemRasterPass pass{
             .Name        = "SkyboxPass",
             .ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
             { return Render( context ); },
             .TargetFramebuffer = targetFb };
        pass.Declare = [this, depth]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        { DeclareSkyDraw( declared, refs, depth ); };
        return pass;
    }

    void SkyboxRenderer::DeclareSkyDraw( RenderPassDeclaration& declared, const FrameGraphRefs& refs,
                                         const SkyDepth depth )
    {
        m_SkyDraw = {};
        const bool behind = depth == SkyDepth::BehindScene;
        if ( !m_BackdropVisible )
            return;

        // Engine-generated procedural atmosphere (no HDR asset needed). It samples the transmittance and sky-view
        // LUTs: last frame's, imported by SceneRenderer::ImportSceneViewTextures when SkyPassSamplesLuts, or
        // System.White until an earlier frame's SkyAtmosphereLuts nodes have written them (the gradient branch
        // never samples them). Both are entries of the pass's block, so each entry is the read's declaration.
        // The camera block is the frame's view (declared inside SceneRenderer::OnUpdate, so it is there).
        const ViewFrame* view = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        if ( m_UseProceduralSky && m_ProceduralPipeline && m_ProceduralMaterial && m_ActiveCamera != nullptr &&
             view != nullptr )
        {
            m_ProceduralMaterial->Update( view, m_SkyParams );
            const MaterialExecutor* executor = m_ProceduralMaterial->GetMaterialExecutor();
            const RDG::TextureRef   white    = refs.System.White;
            declared.Prepares( *executor );
            declared
                 .Bindings( m_ProceduralLayout.Get( m_ProceduralPipeline->GetSpecification().Shader ),
                            executor->GetRouteFill() )
                 .Texture( SkyboxProceduralShaderParameters::TransmittanceLutTexture,
                           refs.Transients.SkyTransmittanceLut.IsValid() ? refs.Transients.SkyTransmittanceLut
                                                                         : white,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Sampler( SkyboxProceduralShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                 .Texture( SkyboxProceduralShaderParameters::SkyViewLutTexture,
                           refs.Transients.SkyViewLut.IsValid() ? refs.Transients.SkyViewLut : white,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 // Black, not white, when absent: the shader reads it only with a light 1 present, and a light 1
                 // whose LUT is not filled yet must add nothing for that frame rather than a white dome.
                 .Texture( SkyboxProceduralShaderParameters::SkyViewLut1Texture,
                           refs.Transients.SkyViewLut1.IsValid() ? refs.Transients.SkyViewLut1 : refs.System.Black,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() );
            m_SkyDraw = { .Pipeline = behind ? m_ProceduralBehindPipeline.get() : m_ProceduralPipeline.get(),
                          .Executor = executor };
            return;
        }

        // The cubemap is the skybox material's own (Properties): the block is the material route alone.
        if ( const auto material = m_MaterialSkybox.lock() )
        {
            if ( !m_Pipeline )
            {
                m_SkyDraw.Fault = "SkyboxPass: a skybox material without its pipeline";
                return;
            }
            if ( m_ActiveCamera != nullptr )
                material->BindInputs( { view, m_SkyboxLook } );
            const MaterialExecutor* executor = material->GetMaterialExecutor();
            declared.Prepares( *executor );
            declared.Bindings( m_SkyLayout.Get( m_Pipeline->GetSpecification().Shader ),
                               executor->GetRouteFill() );
            m_SkyDraw = { .Pipeline = behind ? m_BehindPipeline.get() : m_Pipeline.get(), .Executor = executor };
        }
    }

    Common::BoolResultStr SkyboxRenderer::Render( const RDG::PassContext& context ) const
    {
        if ( m_SkyDraw.Fault != nullptr )
            return Common::MakeError( m_SkyDraw.Fault );
        if ( m_SkyDraw.Pipeline == nullptr )
            return BOOLSUCCESS;
        // Through the block DeclareSkyDraw declared for the draw it chose.
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_SkyDraw.Pipeline, m_SkyDraw.Executor );
    }

} // namespace Desert::Graphic::System

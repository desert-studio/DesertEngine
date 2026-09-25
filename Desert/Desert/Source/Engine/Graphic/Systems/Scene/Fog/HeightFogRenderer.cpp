#include "HeightFogRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/RenderGraphSort.hpp>
#include <Engine/Graphic/RenderPhase.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

namespace Desert::Graphic::System
{
    namespace
    {
        // 8x8, like every compute pass in the engine: 64 invocations is inside every implementation's
        // guaranteed maximum, and the dispatch bounds-checks, so any target size is fine.
        constexpr uint32_t kWorkGroupSize = 8;

        constexpr const char* kFogShaderName   = "HeightFog";
        constexpr const char* kApplyShaderName = "HeightFogApply";

        constexpr uint32_t GroupCount( uint32_t extent )
        {
            return ( extent + kWorkGroupSize - 1 ) / kWorkGroupSize;
        }

        double BytesToMiB( uint64_t bytes )
        {
            return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
        }
    } // namespace

    HeightFogRenderer::~HeightFogRenderer() = default;

    Common::BoolResultStr HeightFogRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError( "HeightFogRenderer: the fog shaders could not be resolved "
                                      "(HeightFog / HeightFogApply)" );

        // Non-persistent, so the driver keeps one copy per (frame x recording renderer slot) — the
        // Docs/RENDERER_FRAME_STATE.md rule; a shared buffer would let a preview renderer overwrite the
        // viewport's fog mid-frame.
        m_ParamsBuffer = ShaderResources::StorageBuffer::Create( "FogParams", kFogPayloadBytes, kFogParamsBinding,
                                                                 /*persistent=*/false );
        if ( !m_ParamsBuffer )
            return Common::MakeError( "HeightFogRenderer: could not create the fog parameter buffer" );

        m_ApplyMaterial = std::make_unique<MaterialHeightFog>();
        return BOOLSUCCESS;
    }

    bool HeightFogRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return false;

        const auto fogShader = shaderService->GetByName( kFogShaderName );
        if ( !fogShader )
        {
            LOG_ERROR( "[HeightFog] Compute shader '{}' is not registered. Expected "
                       "Editor/Resources/Shaders/Programs/Fog/{}.shader.",
                       kFogShaderName, kFogShaderName );
            return false;
        }
        const auto fog = ComputePipeline::Create( { .Shader = fogShader, .DebugName = kFogShaderName } );
        if ( !fog )
        {
            LOG_ERROR( "[HeightFog] {}", fog.GetError() );
            return false;
        }
        m_FogPipeline = fog.GetValue();

        const auto target = m_TargetFramebuffer.lock();
        if ( !target )
            return false;

        const auto applyShader = shaderService->GetByName( kApplyShaderName );
        if ( !applyShader )
        {
            LOG_ERROR( "[HeightFog] Graphics shader '{}' is not registered.", kApplyShaderName );
            return false;
        }

        GraphicsPipelineSpecification spec;
        spec.DebugName   = kApplyShaderName;
        spec.Shader      = applyShader;
        spec.Framebuffer = target;

        // A fullscreen quad has no meaningful depth of its own; occlusion was resolved inside the
        // compute pass, which evaluated every pixel at the distance the depth attachment reported.
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.CullMode          = CullMode::None;
        spec.Topology          = PrimitiveTopology::Triangles;

        // scene = fog.rgb * One + scene * fog.a — the premultiplied over-operator; the compute pass
        // emits exactly that pair.
        spec.BlendEnable         = true;
        spec.SrcColorBlendFactor = BlendFactor::One;
        spec.DstColorBlendFactor = BlendFactor::SrcAlpha;

        // Replayed by ExecuteTransparency with a LOAD begin, so the pipeline is built against the
        // framebuffer's LOAD render pass.
        spec.UseLoadRenderPass = true;

        const auto apply = GraphicsPipeline::Create( spec );
        if ( !apply )
        {
            LOG_ERROR( "[HeightFog] the apply pipeline was not built: {}", apply.GetError() );
            return false;
        }
        m_ApplyPipeline = apply.GetValue();

        // `return m_FogPipeline && m_ApplyPipeline;` stood here: both were assigned from a Result already
        // checked at the line above, so it could not be false. The refusals are at their own sites now.
        return true;
    }

    void HeightFogRenderer::SetFogSettings( bool present, const ECS::ExponentialHeightFogData& data,
                                            float fogHeightY )
    {
        m_Present    = present;
        m_Data       = data;
        m_FogHeightY = fogHeightY;
    }

    bool HeightFogRenderer::EnsureResources( uint32_t width, uint32_t height )
    {
        if ( m_ResourcesFailed )
            return false;

        if ( m_FogImage && width == m_FogWidth && height == m_FogHeight )
            return true;

        // The old image may still be referenced by descriptors of frames in flight — the same rule the
        // scatter target follows on resize.
        if ( m_FogImage )
            Renderer::GetInstance().WaitDeviceIdle();

        const Core::Formats::Image2DSpecification spec{
             .Tag        = "HeightFogApply",
             .Width      = width,
             .Height     = height,
             .Format     = ViewTargetFormats::kHeightFog,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };

        m_FogImage = Image2D::Create( spec );
        if ( !m_FogImage )
        {
            LOG_ERROR(
                 "[HeightFog] The {}x{} RGBA16F fog target ({:.2f} MiB) could not be created; the "
                 "height fog will not render for this view.",
                 width, height,
                 BytesToMiB( Core::Formats::CalculateImageSize( width, height, ViewTargetFormats::kHeightFog ) ) );
            m_ResourcesFailed = true;
            return false;
        }

        m_FogWidth  = width;
        m_FogHeight = height;

        // The cost is announced once, on the allocation, not discovered in a memory graph later.
        LOG_INFO( "[HeightFog] Fog target {}x{} RGBA16F ({:.2f} MiB) for a {}x{} view.", width, height,
                  BytesToMiB( Core::Formats::CalculateImageSize( width, height, ViewTargetFormats::kHeightFog ) ),
                  width, height );
        return true;
    }

    void HeightFogRenderer::ExecuteInFrame()
    {
        DESERT_PROFILE_PASS( "HeightFog: ExecuteInFrame" );

        m_HasFrameResult = false;

        if ( !m_FogPipeline || !m_ApplyPipeline || !m_ParamsBuffer )
            return;

        // What this dispatch has to evaluate. The two halves are independent: a scene can have fog and no
        // atmosphere, a physical atmosphere and no fog component, or both. The volume handle is null
        // exactly when there is no aerial perspective this frame (AtmosphereEnv's own contract), which
        // includes every SkyModel::ArtisticGradient scene.
        const AtmosphereEnv& atmosphere = m_SceneRenderer->GetAtmosphere();

        const bool fogActive = m_Present && m_Data.Enabled;
        const bool apActive  = atmosphere.AerialPerspectiveVolume != nullptr;

        // The zero-cost contract, now stated over both halves: a scene with neither leaves here, before
        // any allocation, upload or dispatch, and the frame is what it was before this system existed.
        if ( !fogActive && !apActive )
            return;

        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return;

        const auto target = m_TargetFramebuffer.lock();
        if ( !target || target->GetDepthAttachmentCount() == 0 )
            return;

        if ( !EnsureResources( target->GetFramebufferWidth(), target->GetFramebufferHeight() ) )
            return;

        // The atmosphere is a coupling, not a dependency: without one the fog keeps its authored colour
        // and drops the sun lobe and the sky ambient (PackFogParams says so per term). Fog on a
        // sky-less scene is legitimate, so there is no bail-out here.
        const FogGpuPayload payload = PackFogParams( m_Data, atmosphere, m_FogHeightY );
        const auto uploaded = m_ParamsBuffer->SetData( &payload, static_cast<uint32_t>( sizeof( payload ) ) );
        if ( !uploaded.IsSuccess() )
        {
            // The push constants below carry the camera and the pass would run with THIS frame's
            // camera against LAST frame's fog block — a height and a density belonging to a different
            // moment, which reads as fog sliding relative to the world rather than as a missing effect.
            LOG_ERROR( "[Fog] the fog pass does not run this frame, its parameters were not uploaded: {}",
                       uploaded.GetError() );
            return;
        }

        FogPush push{};
        push.InverseViewProjection = glm::inverse( camera->GetProjectionMatrix() * camera->GetViewMatrix() );
        push.CameraPosition        = glm::vec4( camera->GetPosition(), 0.0f );
        push.AerialPerspective =
             glm::vec4( atmosphere.AerialPerspectiveDepthKm, atmosphere.AerialPerspectiveViewDistanceScale,
                        apActive ? 1.0f : 0.0f, fogActive ? 1.0f : 0.0f );

        auto& renderer = Renderer::GetInstance();

        Image2D* depthImage = target->GetDepthAttachmentImage().get();

        // Present the DEPTH attachment to a compute sampler and hand it back afterwards — its tracked
        // layout is not legal for a combined image sampler, and SetInput binds the tracked layout
        // verbatim.
        renderer.ComputeImageBeginRead( depthImage );
        renderer.ComputeImageBeginWrite( m_FogImage.get() );

        m_FogPipeline->SetOutput( kFogOutputBinding, m_FogImage.get(), 0 );
        m_FogPipeline->SetStorageBuffer( kFogParamsBinding, m_ParamsBuffer.get() );
        m_FogPipeline->SetInput( kFogSceneDepthBinding, depthImage );

        // ALWAYS bound, even when the shader will not read it: a `sampler3D` with no image is an invalid
        // descriptor set, not an unused one, and ComputePipeline refuses to dispatch at all when a volume
        // input has no view — so a fog-only scene would silently lose its fog. The engine's 1x1x1 volume
        // fallback is what stands in; push.AerialPerspective.z is 0, so it is never sampled.
        m_FogPipeline->SetInput(
             kFogAerialPerspectiveBinding,
             apActive ? atmosphere.AerialPerspectiveVolume
                      : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get() );
        // The distant sky light, on exactly the same terms as the volume above — always bound, read only
        // when the payload's Ambient.w says the texel is real (PackFogParams sets that from this same
        // handle, so the two cannot disagree).
        m_FogPipeline->SetInput(
             kFogDistantSkyLightBinding,
             atmosphere.DistantSkyLight
                  ? atmosphere.DistantSkyLight
                  : FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ).get() );
        m_FogPipeline->SetPushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );

        renderer.DispatchComputeInFrame( m_FogPipeline.get(), GroupCount( m_FogWidth ), GroupCount( m_FogHeight ),
                                         1 );

        renderer.ComputeImageEndWrite( m_FogImage.get() );
        renderer.ComputeImageEndRead( depthImage );

        m_HasFrameResult = true;
    }

    void HeightFogRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        const auto target = m_TargetFramebuffer.lock();
        if ( !target || !m_ApplyPipeline )
            return;

        RenderGraphBuilder::PassConfig config;
        config.Name        = "HeightFogApply";
        config.Phase       = RenderPhase::Transparency;
        config.ExecuteFunc = [this]()
        {
            if ( !m_HasFrameResult || !m_FogImage || !m_ApplyMaterial )
                return;

            m_ApplyMaterial->BindInputs( m_FogImage.get() );
            Renderer::GetInstance().SubmitFullscreenQuad( m_ApplyPipeline.get(),
                                                          m_ApplyMaterial->GetMaterialExecutor() );
        };
        config.PipelineSpec      = m_ApplyPipeline->GetSpecification();
        config.TargetFramebuffer = target;
        config.Dependencies      = { RenderPassDependency( RenderPhase::Geometry ) };

        // The fog is the FLOOR of the Transparency phase: it must land on the opaque scene before the
        // every particle draw over it, so all of them are composited
        // OVER the fogged world. Stated here, on the pass itself, not implied by registration order.
        config.OrderInPhase = RenderPassOrder::AtmosphericFog;

        builder.AddPass( config );
    }
} // namespace Desert::Graphic::System

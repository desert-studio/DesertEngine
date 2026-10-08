#include "HeightFogRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RenderConfig.hpp> // GlobalTextureFilterSampler, VolumeSampler
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

        // A fullscreen triangle has no meaningful depth of its own; occlusion was resolved inside the
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

    std::vector<ComputeNodeDeclaration> HeightFogRenderer::DeclareFrameNodes( RDG::Builder&    graph,
                                                                              FrameTransients& transients )
    {
        std::vector<ComputeNodeDeclaration> nodes;

        if ( !m_FogPipeline || !m_ApplyPipeline || !m_ParamsBuffer )
            return nodes;

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
            return nodes;

        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return nodes;

        const auto target = m_TargetFramebuffer.lock();
        if ( !target || target->GetDepthAttachmentCount() == 0 )
            return nodes;

        const uint32_t fogWidth  = target->GetFramebufferWidth();
        const uint32_t fogHeight = target->GetFramebufferHeight();
        if ( fogWidth == 0 || fogHeight == 0 )
            return nodes;

        // The atmosphere is a coupling, not a dependency: without one the fog keeps its authored colour
        // and drops the sun lobe and the sky ambient (PackFogParams says so per term). Fog on a
        // sky-less scene is legitimate, so there is no bail-out here.
        const FogGpuPayload payload = PackFogParams( m_Data, atmosphere, m_FogHeightY );
        // BUILD TIME: the node exists only when this upload lands; it fills this frame's slot before the graph
        // runs.
        const auto uploaded = m_ParamsBuffer->SetData( &payload, static_cast<uint32_t>( sizeof( payload ) ) );
        if ( !uploaded.IsSuccess() )
        {
            // The push constants below carry the camera and the pass would run with THIS frame's
            // camera against LAST frame's fog block — a height and a density belonging to a different
            // moment, which reads as fog sliding relative to the world rather than as a missing effect.
            LOG_ERROR( "[Fog] the fog pass does not run this frame, its parameters were not uploaded: {}",
                       uploaded.GetError() );
            return nodes;
        }

        FogPush push{};
        push.InverseViewProjection = glm::inverse( camera->GetProjectionMatrix() * camera->GetViewMatrix() );
        push.CameraPosition        = glm::vec4( camera->GetPosition(), 0.0f );
        push.AerialPerspective =
             glm::vec4( atmosphere.AerialPerspectiveDepthKm, atmosphere.AerialPerspectiveViewDistanceScale,
                        apActive ? 1.0f : 0.0f, fogActive ? 1.0f : 0.0f );

        // One Compute node: it samples the scene depth, the aerial-perspective volume and the distant sky light,
        // and writes the fog image the apply (HeightFogApply, a Transparency raster node) samples.
        // Single-sample: the scene depth at MSAA 1, SceneDepthResolved at MSAA > 1 (a sampler2D over a
        // multisampled image is invalid). With a depth target it always exists: at MSAA > 1 the resolve is part
        // of the renderer, and a resolve that failed to build said so at startup and is said again here.
        const std::shared_ptr<Image2D> depth = m_SceneRenderer->GetComputeSceneDepth();
        if ( !depth )
        {
            LOG_ERROR( "[Fog] the fog pass has no single-sample scene depth: SceneDepthResolve was not "
                       "built for the multisampled scene target (see the startup error)" );
            return {};
        }
        // The fog image lives this frame only (written here, sampled by HeightFogApply in the Transparency
        // phase): a transient of the graph at this frame's view size, published for the apply.
        const RDG::TextureDesc fogDesc{ .Size   = RDG::Extent3D{ .Width = fogWidth, .Height = fogHeight },
                                        .Format = ViewTargetFormats::kHeightFog };
        const RDG::TextureRef  fogImage = graph.CreateTexture( fogDesc, "HeightFog.Fog" );
        ComputeNodeDeclaration fog;
        fog.Name = "AtmosphericFog";
        // SETUP: the parameter buffer is the renderer's own, set on the pipeline route here; every image is an
        // entry of the node's one block (block 0), and each entry is the declaration of its read or write.
        m_FogPipeline->SetStorageBuffer( kFogParamsBinding, m_ParamsBuffer.get() );

        // The sky's two images, ALWAYS entries even when the shader will not read them: a declared sampler with
        // no image is an invalid descriptor set, not an unused one, and ComputePipeline refuses to dispatch when
        // a volume input has no view, so a fog-only scene would silently lose its fog. When the sky publishes
        // null the engine fallback stands in; push.AerialPerspective.z (the volume) and the payload's Ambient.w
        // (the distant sky light, PackFogParams sets it from this same handle) say it is never sampled. Each is
        // read with the sampler it carried as its own image: VolumeSampler() for the 3D volume,
        // GlobalTextureFilterSampler() for the 2D images.
        const std::shared_ptr<Image> aerialPerspective =
             apActive ? std::shared_ptr<Image>( atmosphere.AerialPerspectiveVolume )
                      : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F );
        const std::shared_ptr<Image> distantSkyLight =
             atmosphere.DistantSkyLight
                  ? std::shared_ptr<Image>( atmosphere.DistantSkyLight )
                  : FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F );
        auto block = fog.Access.Bindings( m_FogLayout.Get( m_FogPipeline->GetShader() ),
                                          Renderer::GetPipelineRouteFill( *m_FogPipeline ) );
        block.PushConstantBytes( static_cast<uint32_t>( sizeof( FogPush ) ) );
        block.Storage( "u_FogApply", fogImage, RDG::Access::StorageWrite )
             .Sampled( "u_SceneDepth", depth, RDG::Access::SampledCompute, GlobalTextureFilterSampler(),
                       "SceneDepth.Compute" )
             .Sampled( "u_AerialPerspective", aerialPerspective, RDG::Access::SampledCompute, VolumeSampler(),
                       apActive ? "Sky.AerialPerspectiveLut" : "HeightFog.AerialPerspectiveFallback" )
             .Sampled( "u_DistantSkyLight", distantSkyLight, RDG::Access::SampledCompute,
                       GlobalTextureFilterSampler(),
                       atmosphere.DistantSkyLight ? "Sky.DistantLight" : "HeightFog.DistantSkyLightFallback" );
        fog.Record = [this, push, fogWidth, fogHeight]( RDG::PassContext& context,
                                                        const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            DESERT_PROFILE_PASS( "HeightFog: ExecuteInFrame" );
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            return Renderer::DispatchCompute( bindings, *m_FogPipeline, GroupCount( fogWidth ),
                                              GroupCount( fogHeight ), 1 );
        };
        nodes.push_back( std::move( fog ) );

        transients.HeightFog = fogImage; // -> HeightFogApply (u_FogApply)
        return nodes;
    }

    void HeightFogRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        const auto target = m_TargetFramebuffer.lock();
        if ( !target || !m_ApplyPipeline )
            return;

        RenderGraphBuilder::PassConfig config;
        config.Name        = "HeightFogApply";
        config.Phase       = RenderPhase::Transparency;
        config.ExecuteFunc = [this]( RDG::PassContext&     context,
                                     const FrameGraphRefs& refs ) -> Common::BoolResultStr
        {
            // No fog image this frame (neither fog nor aerial perspective, or the evaluation did not run):
            // the over-composite would be the identity, so nothing is drawn.
            if ( !refs.Transients.HeightFog.IsValid() )
                return BOOLSUCCESS;

            // Through the block Declare declared below.
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_ApplyPipeline, nullptr );
        };
        config.PipelineSpec      = m_ApplyPipeline->GetSpecification();
        config.TargetFramebuffer = target;
        config.Dependencies      = { RenderPassDependency( RenderPhase::Geometry ) };

        // The fog is the FLOOR of the Transparency phase: it must land on the opaque scene before the
        // every particle draw over it, so all of them are composited
        // OVER the fogged world. Stated here, on the pass itself, not implied by registration order.
        config.OrderInPhase = RenderPassOrder::AtmosphericFog;
        // The apply samples the fog image the AtmosphericFog node wrote as a storage image this frame.
        // Its one block: the shader's layout, no other route (no material), and the fog image as the entry
        // that declares the read. texelFetch at the target's own size: the sampler never filters.
        config.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            if ( !refs.Transients.HeightFog.IsValid() )
            {
                return;
            }
            declared.Bindings( m_ApplyLayout.Get( m_ApplyPipeline->GetShader() ), RDG::OtherRouteFill{} )
                 .Sampled( "u_FogApply", refs.Transients.HeightFog, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() );
        };

        builder.AddPass( config );
    }
} // namespace Desert::Graphic::System

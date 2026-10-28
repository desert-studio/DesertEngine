#include <Engine/Graphic/GlobalIllumination/SurfaceCacheRenderer.hpp>

#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/DeferredLightingRenderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <cstring>
#include <format>
#include <span>
#include <utility>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SurfaceCacheCopyShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, CaptureAlbedoTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, CaptureNormalTexture );
        DESERT_SHADER_PARAMETER( Texture, CaptureEmissiveTexture );
        DESERT_SHADER_PARAMETER( Texture, CaptureDepthTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardPageCopies );
        DESERT_SHADER_PARAMETER( Texture, CaptureShadingWordTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SurfaceCacheAtlasViewShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SurfaceCacheSceneViewShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardPageBuffer );
        DESERT_SHADER_PARAMETER( Texture, AtlasColorTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, AtlasDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, AtlasOpacityTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SurfaceCacheDirectLightingShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardPageBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceLightingPages );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCacheShadowUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCacheCloudShadowUB );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, AtlasIndirectLightingTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct SurfaceCacheIndirectLightingShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardPageBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceLightingPages );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, AtlasDirectLightingTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, RadiosityTraceTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp). The cards a hit reads (and the
    // probes stand on) are GlobalIllumination::DeclareSurfaceCacheReads', the tracer's DeclareTracerReads'.
    struct SurfaceCacheRadiosityTraceShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceLightingPages );
        DESERT_SHADER_PARAMETER( Texture, AtlasNormalTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridWorldCellsUB );
        DESERT_SHADER_PARAMETER( Texture, EnvironmentSpecularTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferEmissiveTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
    };
} // namespace

namespace Desert::Graphic::SurfaceCache
{
    namespace
    {
        constexpr std::array<std::string_view, SurfaceCacheRenderer::kCapturedLayers.size()> kLayerNames{
             "SurfaceCache.Depth",  "SurfaceCache.Albedo",   "SurfaceCache.Opacity",
             "SurfaceCache.Normal", "SurfaceCache.Emissive", "SurfaceCache.Subsurface" };
        // The shader names of the atlas layers, in kCapturedLayers order (Programs/SurfaceCache/*.shader).
        constexpr std::array<std::string_view, SurfaceCacheRenderer::kCapturedLayers.size()> kLayerImages{
             "AtlasDepthTexture",  "AtlasAlbedoTexture",   "AtlasOpacityTexture",
             "AtlasNormalTexture", "AtlasEmissiveTexture", "AtlasSubsurfaceTexture" };
        constexpr auto kLayerStorage  = RDG::ParametersOf<RDG::ShaderResourceKind::StorageTexture>( kLayerImages );
        constexpr auto kLayerTextures = RDG::ParametersOf<RDG::ShaderResourceKind::Texture>( kLayerImages );
        constexpr uint32_t kCopyGroup = 8; // Programs/SurfaceCache/SurfaceCacheCardCopy.shader LocalSize

        constexpr std::array<std::string_view, 3> kLitLayerNames{
             "SurfaceCache.DirectLighting", "SurfaceCache.IndirectLighting", "SurfaceCache.FinalLighting" };

        // Programs/SurfaceCache/SurfaceCacheDirectLighting.shader SurfaceDirectLightingPush.
        struct DirectLightingPush
        {
            glm::vec4  LightDir{ 0.0f };
            glm::vec4  LightColor{ 0.0f };
            glm::vec4  Light1Dir{ 0.0f };   // atmosphere light 1 (the moon)
            glm::vec4  Light1Color{ 0.0f }; // a == 0: none
            glm::uvec4 Flags{ 0u }; // x = 1: the pages keep their IndirectLighting (it runs this frame)
        };
        static_assert( sizeof( DirectLightingPush ) == 80 );

        // Programs/SurfaceCache/SurfaceCacheRadiosityTrace.shader SurfaceRadiosityPush.
        struct RadiosityTracePush
        {
            glm::uvec4 Frame{ 0u };     // x = frame index (the strata's jitter), y = 1: no history
            glm::vec4  Weights{ 0.0f }; // x = the new sample's weight against the history
        };
        static_assert( sizeof( RadiosityTracePush ) == 32 );

        // UE r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated: a relit texel's new sample weighs 1 / this.
        constexpr float    kRadiosityMaxFramesAccumulated = 4.0f;
        constexpr std::string_view kRadiosityTraceName    = "SurfaceCache.RadiosityTrace";
        constexpr std::string_view kRadiosityShaderName   = "SurfaceCacheRadiosityTrace";

        struct SceneViewPush
        {
            glm::mat4  ViewProjection{ 1.0f };
            glm::vec4  CameraPosition{ 0.0f };
            glm::uvec4 Counts{ 0u }; // x = atlas side, y = card count
        };
        static_assert( sizeof( SceneViewPush ) == 96 );

        // A graph buffer of @p data; an empty list is uploaded as one zero element nobody reads (an upload
        // refuses an empty payload).
        template <typename T>
        RDG::BufferRef UploadBuffer( RDG::Builder& graph, const std::vector<T>& data, const std::string_view name )
        {
            static const T           zero{};
            const std::span<const T> items =
                 data.empty() ? std::span<const T>( &zero, 1 ) : std::span<const T>( data.data(), data.size() );
            const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ items.size_bytes() }, name );
            graph.QueueBufferUpload( buffer, std::as_bytes( items ) );
            return buffer;
        }
    } // namespace

    Common::BoolResultStr SurfaceCacheRenderer::Initialize()
    {
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        m_CopyShader       = shaders->GetByName( "SurfaceCacheCardCopy" );
        m_AtlasViewShader  = shaders->GetByName( "SurfaceCacheAtlasView" );
        m_SceneViewShader  = shaders->GetByName( "SurfaceCacheSceneView" );
        // The direct lighting is compiled per tracer (its shadow rays): EnsureLightingProgram, at its first pass.
        m_LightingShader = shaders->GetByName( "SurfaceCacheDirectLighting" );
        m_IndirectShader = shaders->GetByName( "SurfaceCacheIndirectLighting" );
        for ( const auto& [shader, name] :
              { std::pair{ m_CopyShader.get(), "SurfaceCacheCardCopy" },
                std::pair{ m_AtlasViewShader.get(), "SurfaceCacheAtlasView" },
                std::pair{ m_SceneViewShader.get(), "SurfaceCacheSceneView" },
                std::pair{ m_LightingShader.get(), "SurfaceCacheDirectLighting" },
                std::pair{ m_IndirectShader.get(), "SurfaceCacheIndirectLighting" } } )
            if ( shader == nullptr )
                return Common::MakeFormattedError<bool>( "surface cache: shader {} not found", name );
        auto pipeline = ComputePipeline::Create( { .Shader = m_CopyShader, .DebugName = "SurfaceCacheCardCopy" } );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        m_CopyPipeline = pipeline.GetValue();

        // The indirect lighting: one quad per relit page into IndirectLighting and FinalLighting.
        {
            GraphicsPipelineSpecification indirect;
            indirect.DebugName = "SurfaceCacheIndirectLighting";
            indirect.TargetLayout =
                 RenderTargetLayout{ .ColorFormats = { kLayerFormats[static_cast<std::size_t>( kLitLayers[1] )],
                                                       kLayerFormats[static_cast<std::size_t>( kLitLayers[2] )] },
                                     .DepthFormat  = std::nullopt,
                                     .Samples      = 1 };
            indirect.Shader             = m_IndirectShader;
            indirect.DepthTestEnabled   = false;
            indirect.DepthWriteEnabled  = false;
            const auto indirectPipeline = GraphicsPipeline::Create( indirect );
            if ( !indirectPipeline )
                return Common::MakeError<bool>( indirectPipeline.GetError() );
            m_IndirectPipeline = indirectPipeline.GetValue();
        }

        // The capture atlas: the G-buffer's slots (the view-pass cells' outputs), its own fixed side.
        FramebufferSpecification spec;
        spec.DebugName = "SurfaceCacheCapture";
        // The G-buffer's own attachments (the view-pass cell's pipeline is built against GBufferLayout); the
        // velocity slot is the capture pass's graph transient (SceneRenderer::AddFrameSurfaceCacheView).
        for ( const Core::Formats::ImageFormat format :
              { ViewTargetFormats::kGBufferA, ViewTargetFormats::kGBufferB, ViewTargetFormats::kGBufferShadingWord,
                ViewTargetFormats::kGBufferEmissive } )
            spec.Attachments.Attachments.emplace_back( format );
        spec.Attachments.Attachments.emplace_back( ViewTargetFormats::kGBufferDepth );
        std::shared_ptr<Framebuffer> captureBuffer = Framebuffer::Create( spec );
        if ( !captureBuffer )
            return Common::MakeError<bool>( "surface cache: the capture framebuffer could not be created" );
        if ( const auto resized = captureBuffer->Resize( kCaptureAtlasSize, kCaptureAtlasSize ); !resized )
            return Common::MakeFormattedError<bool>( "surface cache: capture atlas {}²: {}", kCaptureAtlasSize,
                                                     resized.GetError() );
        // Last: IsInitialized() reads it, so a renderer is initialised only once everything is.
        m_CaptureBuffer = std::move( captureBuffer );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SurfaceCacheRenderer::Configure( const SurfaceCacheBudget& budget )
    {
        m_Scene.Configure( budget, kCaptureAtlasSize, kMaxCapturesPerFrame );
        if ( budget.AtlasSize == m_AtlasSize )
            return BOOLSUCCESS;
        if ( budget.AtlasSize < kPhysicalPageSize )
            return Common::MakeFormattedError<bool>( "surface cache: atlas side {} is below one page ({})",
                                                     budget.AtlasSize, kPhysicalPageSize );
        for ( std::size_t i = 0; i < kCapturedLayers.size(); ++i )
        {
            const Core::Formats::ImageFormat format =
                 kLayerFormats[static_cast<std::size_t>( kCapturedLayers[i] )];
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = std::string( kLayerNames[i] ),
                 .Width      = budget.AtlasSize,
                 .Height     = budget.AtlasSize,
                 .Format     = format,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_Layers[i] = Image2D::Create( spec );
            if ( !m_Layers[i] )
                return Common::MakeFormattedError<bool>( "surface cache: atlas layer {} ({}²) not created",
                                                         kLayerNames[i], budget.AtlasSize );
        }
        for ( std::size_t i = 0; i < kLitLayers.size(); ++i )
        {
            // Colour targets of the direct lighting, sampled by the views (and the tracers later).
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = std::string( kLitLayerNames[i] ),
                 .Width      = budget.AtlasSize,
                 .Height     = budget.AtlasSize,
                 .Format     = kLayerFormats[static_cast<std::size_t>( kLitLayers[i] )],
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Attachment,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_LitLayers[i] = Image2D::Create( spec );
            if ( !m_LitLayers[i] )
                return Common::MakeFormattedError<bool>( "surface cache: atlas layer {} ({}²) not created",
                                                         kLitLayerNames[i], budget.AtlasSize );
        }
        {
            // The radiosity trace atlas: a colour target of the radiosity trace, sampled by its integration.
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = std::string( kRadiosityTraceName ),
                 .Width      = budget.AtlasSize,
                 .Height     = budget.AtlasSize,
                 .Format     = Core::Formats::ImageFormat::R11G11B10F,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Attachment,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_RadiosityTrace = Image2D::Create( spec );
            if ( !m_RadiosityTrace )
                return Common::MakeFormattedError<bool>( "surface cache: atlas layer {} ({}²) not created",
                                                         kRadiosityTraceName, budget.AtlasSize );
            m_ResetRadiosity = true;
        }
        m_AtlasSize = budget.AtlasSize;
        return BOOLSUCCESS;
    }

    Common::ResultStr<SurfaceCacheRenderer::SceneUpdate>
    SurfaceCacheRenderer::UpdateScene( const std::vector<SceneInstance>& instances, const glm::vec3& camera,
                                       const uint64_t                                 engineFrame,
                                       const GlobalIllumination::DirectionalLighting& lighting )
    {
        // What this call asks for: the camera and the instance list's identity (key, placement, surface and
        // capture readiness of each, in order) - the once-per-frame rule compares it with the frame's update.
        uint64_t inputs = 0xCBF29CE484222325ULL ^ static_cast<uint64_t>( instances.size() );
        for ( const auto& instance : instances )
        {
            inputs = ( inputs ^ instance.Key ) * 0x100000001B3ULL;
            inputs = ( inputs ^ SurfaceCacheScene::PlacementKey( instance.Mesh, instance.World ) ) * 0x100000001B3ULL;
            inputs = ( inputs ^ instance.SurfaceKey ) * 0x100000001B3ULL;
            inputs = ( inputs ^ ( instance.CaptureReady ? 1ULL : 0ULL ) ) * 0x100000001B3ULL;
        }
        if ( m_FrameUpdate && m_FrameUpdate->Frame == engineFrame )
        {
            if ( camera != m_FrameUpdate->Camera || inputs != m_FrameUpdate->Instances )
                return Common::MakeFormattedError<SceneUpdate>(
                     "surface cache: updated once per frame - frame {} was updated for another camera or instance "
                     "set (its captures and relit pages are what that frame's earlier graph writes and reads)",
                     engineFrame );
            // The frame's update already ran: this graph reads the same cache, nothing is updated.
            return Common::MakeSuccess( SceneUpdate{ .Scene = m_FrameUpdate->Scene, .Updated = false } );
        }

        if ( !m_LitBy || GlobalIllumination::InvalidatesLightingHistory( *m_LitBy, lighting ) )
        {
            if ( m_LitBy )
                m_Scene.InvalidateLighting();
            m_LitBy          = lighting;
            m_ResetRadiosity = true;
        }
        SceneFrame scene = m_Scene.Update( instances, camera );
        m_FrameUpdate    = FrameUpdate{ .Frame = engineFrame, .Camera = camera, .Instances = inputs, .Scene = scene };
        return Common::MakeSuccess( SceneUpdate{ .Scene = std::move( scene ), .Updated = true } );
    }

    Common::BoolResultStr SurfaceCacheRenderer::AddCopyPass( RDG::Builder& graph, const ImportImage& import,
                                                             const CaptureTextures& capture,
                                                             const SceneFrame&      frame )
    {
        if ( frame.Copies.empty() )
            return BOOLSUCCESS;
        std::array<RDG::TextureRef, kCapturedLayers.size()> layers;
        for ( std::size_t i = 0; i < layers.size(); ++i )
            layers[i] = import( m_Layers[i], kLayerNames[i] );
        const RDG::BufferRef copies   = UploadBuffer( graph, frame.Copies, "SurfaceCache.PageCopies" );
        ComputePipeline*     pipeline = m_CopyPipeline.get();
        const uint32_t       count    = static_cast<uint32_t>( frame.Copies.size() );
        graph.AddPass(
             std::format( "SurfaceCache: Copy Cards ({} pages of {} captures)", count, frame.Captures.size() ),
             RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 auto block = pass.Bindings( m_CopyLayout.Get( m_CopyShader ),
                                             Renderer::GetPipelineRouteFill( *pipeline ) );
                 block.Texture( SurfaceCacheCopyShaderParameters::CaptureAlbedoTexture, capture.Albedo,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( SurfaceCacheCopyShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( SurfaceCacheCopyShaderParameters::CaptureNormalTexture, capture.Normal,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( SurfaceCacheCopyShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( SurfaceCacheCopyShaderParameters::CaptureShadingWordTexture, capture.ShadingWord,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( SurfaceCacheCopyShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( SurfaceCacheCopyShaderParameters::CaptureEmissiveTexture, capture.Emissive,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( SurfaceCacheCopyShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( SurfaceCacheCopyShaderParameters::CaptureDepthTexture, capture.Depth,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( SurfaceCacheCopyShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Storage( SurfaceCacheCopyShaderParameters::SurfaceCardPageCopies, copies,
                                RDG::Access::StorageRead );
                 for ( std::size_t i = 0; i < layers.size(); ++i )
                     block.Storage( kLayerStorage[i], layers[i], RDG::Access::StorageWrite );
             },
             [pipeline, count, scene = &m_Scene,
              frameId = frame.Frame]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 auto dispatched = Renderer::DispatchCompute( bindings, *pipeline, kPhysicalPageSize / kCopyGroup,
                                                              kPhysicalPageSize / kCopyGroup, count );
                 // The captures of this frame landed: their cards are the surface from the next update on.
                 if ( dispatched )
                     scene->ConfirmCapturesExecuted( frameId );
                 return dispatched;
             } );
        return BOOLSUCCESS;
    }

    Common::ResultStr<std::shared_ptr<GraphicsPipeline>> SurfaceCacheRenderer::ViewPipeline(
         std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>>& cache,
         const std::shared_ptr<Shader>& shader, const Core::Formats::ImageFormat format, const uint32_t samples,
         const bool depth, const std::string_view name )
    {
        const auto key = std::make_pair( static_cast<int>( format ), samples );
        if ( const auto it = cache.find( key ); it != cache.end() )
            return Common::MakeSuccess( it->second );
        GraphicsPipelineSpecification spec;
        spec.DebugName    = std::string( name );
        spec.TargetLayout = RenderTargetLayout{
             .ColorFormats = { format },
             .DepthFormat  = depth ? std::optional( Core::Formats::ImageFormat::DEPTH32F ) : std::nullopt,
             .Samples      = samples };
        spec.Shader            = shader;
        spec.DepthTestEnabled  = depth;
        spec.DepthWriteEnabled = depth;
        spec.DepthCompareOp    = DepthCompare::Closer;
        const auto pipeline    = GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( pipeline.GetError() );
        return Common::MakeSuccess( cache.emplace( key, pipeline.GetValue() ).first->second );
    }

    Common::BoolResultStr SurfaceCacheRenderer::AddAtlasViewPass( RDG::Builder& graph, const ImportImage& import,
                                                                  const RDG::TextureRef target )
    {
        const auto desc = graph.GetTextureDesc( target );
        if ( !desc )
            return Common::MakeError<bool>( desc.GetError() );
        const auto pipeline = ViewPipeline( m_AtlasViewPipelines, m_AtlasViewShader, desc.GetValue().Format,
                                            desc.GetValue().Samples, false, "SurfaceCacheAtlasView" );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        std::array<RDG::TextureRef, kCapturedLayers.size()> layers;
        for ( std::size_t i = 0; i < layers.size(); ++i )
            layers[i] = import( m_Layers[i], kLayerNames[i] );
        GraphicsPipeline* const raw = pipeline.GetValue().get();
        graph.AddPass(
             "Debug: Surface Cache Atlas", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 auto block = pass.Bindings( m_AtlasViewLayout.Get( m_AtlasViewShader ), RDG::OtherRouteFill{} );
                 for ( std::size_t i = 0; i < layers.size(); ++i )
                     if ( kCapturedLayers[i] != Layer::Emissive && kCapturedLayers[i] != Layer::Subsurface )
                         block.Texture( kLayerTextures[i], layers[i], RDG::Access::SampledGraphics,
                                        RDG::SubresourceRange::All() )
                              .Sampler( SurfaceCacheAtlasViewShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() );
                 pass.ColorTarget( 0, target, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 1.0f ) );
             },
             [raw]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DrawFullscreen( bindings, *raw, nullptr );
             } );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SurfaceCacheRenderer::AddSceneViewPass( RDG::Builder& graph, const ImportImage& import,
                                                                  const RDG::TextureRef target,
                                                                  const SceneFrame&     frame,
                                                                  const FrameBuffers& buffers, const bool lit,
                                                                  const glm::mat4& viewProjection,
                                                                  const glm::vec3& cameraPosition )
    {
        const auto desc = graph.GetTextureDesc( target );
        if ( !desc )
            return Common::MakeError<bool>( desc.GetError() );
        const auto pipeline = ViewPipeline( m_SceneViewPipelines, m_SceneViewShader, desc.GetValue().Format,
                                            desc.GetValue().Samples, true, "SurfaceCacheSceneView" );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        RDG::TextureDesc depthDesc  = desc.GetValue();
        depthDesc.Format            = Core::Formats::ImageFormat::DEPTH32F;
        depthDesc.Mips              = 1;
        depthDesc.Layers            = 1;
        const RDG::TextureRef depth = graph.CreateTexture( depthDesc, "SurfaceCache.SceneViewDepth" );
        const RDG::BufferRef  cards = buffers.Cards;
        const RDG::BufferRef  pages = buffers.Pages;
        RDG::TextureRef       albedo, depthLayer, opacity;
        for ( std::size_t i = 0; i < kCapturedLayers.size(); ++i )
        {
            const RDG::TextureRef layer = import( m_Layers[i], kLayerNames[i] );
            if ( kCapturedLayers[i] == Layer::Albedo )
                albedo = layer;
            else if ( kCapturedLayers[i] == Layer::Depth )
                depthLayer = layer;
            else if ( kCapturedLayers[i] == Layer::Opacity )
                opacity = layer;
        }
        // The Lit view shows the cards' FinalLighting where the Cards view shows their albedo.
        const RDG::TextureRef color = lit ? import( m_LitLayers[2], kLitLayerNames[2] ) : albedo;
        SceneViewPush         push;
        push.ViewProjection = viewProjection;
        push.CameraPosition = glm::vec4( cameraPosition, 1.0f );
        push.Counts         = glm::uvec4( m_AtlasSize, static_cast<uint32_t>( frame.Cards.size() ), 0u, 0u );
        GraphicsPipeline* const raw   = pipeline.GetValue().get();
        const uint32_t          count = static_cast<uint32_t>( frame.Cards.size() );
        graph.AddPass(
             lit ? "Debug: Lumen Scene (lit surface cache cards)" : "Debug: Lumen Scene (surface cache cards)",
             RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_SceneViewLayout.Get( m_SceneViewShader ), RDG::OtherRouteFill{} )
                      .Storage( SurfaceCacheSceneViewShaderParameters::SurfaceCardBuffer, cards,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheSceneViewShaderParameters::SurfaceCardPageBuffer, pages,
                                RDG::Access::StorageRead )
                      .Texture( SurfaceCacheSceneViewShaderParameters::AtlasColorTexture, color,
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .Sampler( SurfaceCacheSceneViewShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( SurfaceCacheSceneViewShaderParameters::AtlasDepthTexture, depthLayer,
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .Texture( SurfaceCacheSceneViewShaderParameters::AtlasOpacityTexture, opacity,
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( SceneViewPush ) ) );
                 pass.ColorTarget( 0, target, RDG::LoadOp::ClearColor( 0.02f, 0.02f, 0.03f, 1.0f ) );
                 pass.DepthTarget( depth, RDG::LoadOp::ClearDepth( Core::kDepthClear ), true );
             },
             [raw, push, count]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 // Six vertices per card (its front quad), one instance per card; a card not captured collapses.
                 if ( count == 0 )
                     return BOOLSUCCESS;
                 return Renderer::DrawProcedural( bindings, *raw, nullptr, 6u, count );
             } );
        return BOOLSUCCESS;
    }

    SurfaceCacheRenderer::FrameBuffers SurfaceCacheRenderer::UploadRecords( RDG::Builder&     graph,
                                                                            const SceneFrame& frame ) const
    {
        return { .Cards         = UploadBuffer( graph, frame.Cards, "SurfaceCache.Cards" ),
                 .Pages         = UploadBuffer( graph, frame.Pages, "SurfaceCache.Pages" ),
                 .MeshCards     = UploadBuffer( graph, frame.MeshCards, "SurfaceCache.MeshCards" ),
                 .InstanceRows  = UploadBuffer( graph, frame.InstanceRows, "SurfaceCache.InstanceRows" ),
                 .LightingPages = UploadBuffer( graph, frame.LightingPages, "SurfaceCache.LightingPages" ) };
    }

    GlobalIllumination::SurfaceCacheFrame SurfaceCacheRenderer::HitReads( const ImportImage&  import,
                                                                          const FrameBuffers& buffers ) const
    {
        GlobalIllumination::SurfaceCacheFrame reads;
        if ( m_AtlasSize == 0 )
            return reads; // not configured: SurfaceCacheFrameMissing names it
        reads.Cards         = buffers.Cards;
        reads.Pages         = buffers.Pages;
        reads.MeshCards     = buffers.MeshCards;
        reads.InstanceRows  = buffers.InstanceRows;
        reads.Depth         = import( m_Layers[0], kLayerNames[0] );
        reads.Opacity       = import( m_Layers[2], kLayerNames[2] );
        reads.FinalLighting = import( m_LitLayers[2], kLitLayerNames[2] );
        return reads;
    }

    namespace
    {
        constexpr std::string_view kDirectLightingShaderName = "SurfaceCacheDirectLighting";
    } // namespace

    std::vector<Core::CookedProgramVariant> SurfaceCacheRenderer::ProgramVariants()
    {
        std::vector<Core::CookedProgramVariant> variants =
             GlobalIllumination::TracerProgramVariants( kDirectLightingShaderName );
        for ( auto& variant : GlobalIllumination::TracerProgramVariants( kRadiosityShaderName ) )
            variants.push_back( std::move( variant ) );
        return variants;
    }

    Common::BoolResultStr SurfaceCacheRenderer::EnsureRadiosityProgram( const GlobalIllumination::Pipeline& tracer )
    {
        if ( m_RadiosityPipeline && tracer == m_RadiosityProgramFor )
            return BOOLSUCCESS;
        auto shader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant(
             std::string( kRadiosityShaderName ), GlobalIllumination::TracerVariant( tracer ) );
        if ( !shader )
            return Common::MakeFormattedError<bool>( "surface cache: shader {} ({}) did not compile",
                                                     kRadiosityShaderName,
                                                     GlobalIllumination::TracerDefine( tracer.Tracer ) );
        // One quad per relit page into the trace atlas, blended with the history by the sample's weight
        // (src alpha; the target keeps no alpha of its own).
        GraphicsPipelineSpecification spec;
        spec.DebugName =
             std::format( "{} ({})", kRadiosityShaderName, GlobalIllumination::TracerDefine( tracer.Tracer ) );
        spec.TargetLayout        = RenderTargetLayout{ .ColorFormats = { Core::Formats::ImageFormat::R11G11B10F },
                                                       .DepthFormat  = std::nullopt,
                                                       .Samples      = 1 };
        spec.Shader              = shader;
        spec.DepthTestEnabled    = false;
        spec.DepthWriteEnabled   = false;
        spec.BlendEnable         = true;
        spec.SrcColorBlendFactor = BlendFactor::SrcAlpha;
        spec.DstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;
        const auto pipeline      = GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        m_RadiosityShader     = std::move( shader );
        m_RadiosityPipeline   = pipeline.GetValue();
        m_RadiosityProgramFor = tracer;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SurfaceCacheRenderer::EnsureLightingProgram( const GlobalIllumination::Pipeline& tracer )
    {
        if ( m_LightingPipeline && tracer == m_LightingProgramFor )
            return BOOLSUCCESS;
        auto shader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant(
             std::string( kDirectLightingShaderName ), GlobalIllumination::TracerVariant( tracer ) );
        if ( !shader )
            return Common::MakeFormattedError<bool>(
                 "surface cache: shader SurfaceCacheDirectLighting ({}) did not "
                 "compile",
                 GlobalIllumination::TracerDefine( tracer.Tracer ) );
        // One quad per relit page into the two lit layers (no depth, no blend: every texel of a relit page is
        // written).
        GraphicsPipelineSpecification lighting;
        lighting.DebugName =
             std::format( "SurfaceCacheDirectLighting ({})", GlobalIllumination::TracerDefine( tracer.Tracer ) );
        lighting.TargetLayout =
             RenderTargetLayout{ .ColorFormats = { kLayerFormats[static_cast<std::size_t>( kLitLayers[0] )],
                                                   kLayerFormats[static_cast<std::size_t>( kLitLayers[2] )] },
                                 .DepthFormat  = std::nullopt,
                                 .Samples      = 1 };
        lighting.Shader            = shader;
        lighting.DepthTestEnabled  = false;
        lighting.DepthWriteEnabled = false;
        const auto pipeline        = GraphicsPipeline::Create( lighting );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        m_LightingShader     = std::move( shader );
        m_LightingPipeline   = pipeline.GetValue();
        m_LightingProgramFor = tracer;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SurfaceCacheRenderer::AddDirectLightingPass( RDG::Builder&               graph,
                                                                       const ImportImage&          import,
                                                                       const SceneFrame&           frame,
                                                                       const FrameBuffers&         buffers,
                                                                       const DirectLightingInputs& inputs )
    {
        if ( frame.LightingPages.empty() )
            return BOOLSUCCESS;
        if ( !inputs.Lights.Point.IsValid() || !inputs.Lights.Spot.IsValid() ||
             !inputs.Lights.ShadowViews.IsValid() )
            return Common::MakeError<bool>( "surface cache direct lighting: the view's light buffers were not "
                                            "uploaded this frame (AddFrameLightGrid)" );
        if ( !inputs.Lights.WorldGridParams.IsValid() || !inputs.Lights.WorldGridCells.IsValid() )
            return Common::MakeError<bool>(
                 "surface cache direct lighting: the world light grid was not built this "
                 "frame (AddFrameLightGridWorld)" );
        if ( const std::string_view missing = GlobalIllumination::TracerFrameMissing( inputs.Tracer );
             !missing.empty() )
            return Common::MakeFormattedError<bool>(
                 "surface cache direct lighting: {} (its shadow rays off the view's shadow maps)", missing );
        if ( const auto program = EnsureLightingProgram( inputs.Tracer.Pipeline ); !program )
            return program;
        std::array<RDG::TextureRef, kCapturedLayers.size()> layers;
        for ( std::size_t i = 0; i < layers.size(); ++i )
            layers[i] = import( m_Layers[i], kLayerNames[i] );
        const RDG::TextureRef direct   = import( m_LitLayers[0], kLitLayerNames[0] );
        const RDG::TextureRef indirect = import( m_LitLayers[1], kLitLayerNames[1] );
        const RDG::TextureRef finalLit = import( m_LitLayers[2], kLitLayerNames[2] );
        const RDG::BufferRef  relit    = buffers.LightingPages;
        const RDG::BufferRef shadow = UploadBuffer( graph, std::vector{ inputs.Shadow }, "SurfaceCache.ShadowUB" );
        const RDG::BufferRef cloud =
             UploadBuffer( graph, std::vector{ inputs.Cloud }, "SurfaceCache.CloudShadowUB" );

        DirectLightingPush push;
        push.LightDir                 = inputs.LightDir;
        push.LightColor               = inputs.LightColor;
        push.Light1Dir                = inputs.Light1Dir;
        push.Light1Color              = inputs.Light1Color;
        push.Flags.x                  = inputs.IndirectLighting ? 1u : 0u;
        GraphicsPipeline* const raw   = m_LightingPipeline.get();
        const uint32_t          count = static_cast<uint32_t>( frame.LightingPages.size() );
        graph.AddPass(
             std::format( "SurfaceCache: Direct Lighting ({} pages)", count ), RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_LightingLayout.Get( m_LightingShader );
                 auto        block  = pass.Bindings( layout, RDG::OtherRouteFill{} );
                 block.Storage( SurfaceCacheDirectLightingShaderParameters::SurfaceCardBuffer, buffers.Cards,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheDirectLightingShaderParameters::SurfaceCardPageBuffer, buffers.Pages,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheDirectLightingShaderParameters::SurfaceLightingPages, relit,
                                RDG::Access::StorageRead )
                      .Storage( ShaderProtocols::PointLight::Parameter, inputs.Lights.Point,
                                RDG::Access::StorageRead )
                      .Storage( ShaderProtocols::SpotLight::Parameter, inputs.Lights.Spot,
                                RDG::Access::StorageRead )
                      .Storage( kLocalShadowViewsBlock, inputs.Lights.ShadowViews,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheDirectLightingShaderParameters::SurfaceCacheShadowUB, shadow,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheDirectLightingShaderParameters::SurfaceCacheCloudShadowUB, cloud,
                                RDG::Access::StorageRead )
                      .Storage( kLightGridParamsBlock, inputs.Lights.WorldGridParams, RDG::Access::StorageRead )
                      .Storage( kLightGridWorldCellsBlock, inputs.Lights.WorldGridCells, RDG::Access::StorageRead )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( DirectLightingPush ) ) );
                 for ( std::size_t i = 0; i < layers.size(); ++i )
                     block.Texture( kLayerTextures[i], layers[i], RDG::Access::SampledGraphics,
                                    RDG::SubresourceRange::All() )
                          .Sampler( SurfaceCacheDirectLightingShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() );
                 block.Texture( SurfaceCacheDirectLightingShaderParameters::AtlasIndirectLightingTexture, indirect,
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .Sampler( SurfaceCacheDirectLightingShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() );
                 // The active tracer's reads (its visibility rays), sampled in the fragment stage.
                 GlobalIllumination::DeclareTracerReads( block, inputs.Tracer, RDG::Access::SampledGraphics );
                 // The cascades, the cloud map and the local shadow atlas, with the composite's samplers; the
                 // light grid read is the world grid bound above, not the view's.
                 // (BindSceneViewInputs would bind the view's grid under the same block name a second time.)
                 if ( layout )
                     SceneViewDetail::BindWhere( block, inputs.View, [&bound = *layout]( std::string_view name )
                                                 { return SceneViewDetail::LayoutSamples( bound, name ); } );
                 pass.ColorTarget( 0, direct, RDG::LoadOp::Load() );
                 pass.ColorTarget( 1, finalLit, RDG::LoadOp::Load() );
             },
             [raw, push, count]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 // Six vertices per relit page (its atlas rectangle), one instance per page.
                 return Renderer::DrawProcedural( bindings, *raw, nullptr, 6u, count );
             } );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SurfaceCacheRenderer::AddIndirectLightingPass( RDG::Builder&                 graph,
                                                                         const ImportImage&            import,
                                                                         const SceneFrame&             frame,
                                                                         const FrameBuffers&           buffers,
                                                                         const IndirectLightingInputs& inputs )
    {
        if ( frame.LightingPages.empty() )
            return BOOLSUCCESS;
        if ( !inputs.Params.IsValid() || !inputs.Indirection.IsValid() || !inputs.Probes.IsValid() ||
             !inputs.Irradiance.IsValid() || !inputs.Depth.IsValid() )
            return Common::MakeError<bool>(
                 "surface cache indirect lighting: the world radiance cache was not updated this frame" );
        if ( const std::string_view missing = GlobalIllumination::TracerFrameMissing( inputs.Tracer );
             !missing.empty() )
            return Common::MakeFormattedError<bool>( "surface cache radiosity: {}", missing );
        if ( const std::string_view missing = GlobalIllumination::SurfaceCacheFrameMissing( inputs.Hits );
             !missing.empty() )
            return Common::MakeFormattedError<bool>( "surface cache radiosity: {}", missing );
        if ( !inputs.GIFrame.IsValid() || !inputs.PointLights.IsValid() || !inputs.SpotLights.IsValid() ||
             !inputs.WorldGridParams.IsValid() || !inputs.WorldGridCells.IsValid() || !inputs.EnvSpecular.IsValid() ||
             !inputs.GBufferDepth.IsValid() || !inputs.GBufferA.IsValid() || !inputs.GBufferEmissive.IsValid() )
            return Common::MakeError<bool>( "surface cache radiosity: the GI ray contract's reads are incomplete" );
        if ( const auto program = EnsureRadiosityProgram( inputs.Tracer.Pipeline ); !program )
            return program;
        std::array<RDG::TextureRef, kCapturedLayers.size()> layers;
        for ( std::size_t i = 0; i < layers.size(); ++i )
            layers[i] = import( m_Layers[i], kLayerNames[i] );
        const RDG::TextureRef   direct   = import( m_LitLayers[0], kLitLayerNames[0] );
        const RDG::TextureRef   indirect = import( m_LitLayers[1], kLitLayerNames[1] );
        const RDG::TextureRef   finalLit = import( m_LitLayers[2], kLitLayerNames[2] );
        GraphicsPipeline* const raw      = m_IndirectPipeline.get();
        const uint32_t          count    = static_cast<uint32_t>( frame.LightingPages.size() );
        const auto              all      = RDG::SubresourceRange::All();
        const RDG::TextureRef   trace    = import( m_RadiosityTrace, kRadiosityTraceName );

        // The radiosity probes' trace (UE LumenRadiosity's trace), one ray per atlas texel of the relit pages.
        RadiosityTracePush tracePush;
        tracePush.Frame.x   = m_FrameUpdate ? static_cast<uint32_t>( m_FrameUpdate->Frame ) : 0u;
        tracePush.Frame.y   = m_ResetRadiosity ? 1u : 0u;
        tracePush.Weights.x = 1.0f / kRadiosityMaxFramesAccumulated;
        m_ResetRadiosity    = false;
        GraphicsPipeline* const traceRaw = m_RadiosityPipeline.get();
        graph.AddPass(
             std::format( "SurfaceCache: Radiosity Trace ({} pages)", count ), RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 using P            = SurfaceCacheRadiosityTraceShaderParameters;
                 const auto& layout = m_RadiosityLayout.Get( m_RadiosityShader );
                 auto        block  = pass.Bindings( layout, RDG::OtherRouteFill{} );
                 constexpr RDG::Access kR = RDG::Access::StorageRead;
                 constexpr RDG::Access kS = RDG::Access::SampledGraphics;
                 block.Storage( P::SurfaceLightingPages, buffers.LightingPages, kR )
                      .Storage( P::GIFrameUB, inputs.GIFrame, kR )
                      .Storage( P::PointLightsUB, inputs.PointLights, kR )
                      .Storage( P::SpotLightsUB, inputs.SpotLights, kR )
                      .Storage( P::LightGridParamsUB, inputs.WorldGridParams, kR )
                      .Storage( P::LightGridWorldCellsUB, inputs.WorldGridCells, kR )
                      .Storage( P::RadianceCacheParamsUB, inputs.Params, kR )
                      .Storage( P::RadianceCacheIndirectionUB, inputs.Indirection, kR )
                      .Storage( P::RadianceCacheProbesUB, inputs.Probes, kR )
                      .Texture( P::RadianceCacheIrradianceTexture, inputs.Irradiance, kS, all )
                      .Sampler( P::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                      .Texture( P::RadianceCacheDepthTexture, inputs.Depth, kS, all )
                      .Texture( P::EnvironmentSpecularTexture, inputs.EnvSpecular, kS, all )
                      .Texture( P::GBufferDepthTexture, inputs.GBufferDepth, kS, all )
                      .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                      .Texture( P::GBufferATexture, inputs.GBufferA, kS, all )
                      .Texture( P::GBufferEmissiveTexture, inputs.GBufferEmissive, kS, all )
                      .Texture( P::AtlasNormalTexture, layers[3], kS, all )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( RadiosityTracePush ) ) );
                 GlobalIllumination::DeclareSurfaceCacheReads( block, inputs.Hits, kS );
                 GlobalIllumination::DeclareTracerReads( block, inputs.Tracer, kS );
                 pass.ColorTarget( 0, trace, RDG::LoadOp::Load() );
             },
             [traceRaw, tracePush, count]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &tracePush, static_cast<uint32_t>( sizeof( tracePush ) ) );
                 return Renderer::DrawProcedural( bindings, *traceRaw, nullptr, 6u, count );
             } );

        graph.AddPass(
             std::format( "SurfaceCache: Indirect Lighting ({} pages)", count ), RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_IndirectLayout.Get( m_IndirectShader );
                 auto        block  = pass.Bindings( layout, RDG::OtherRouteFill{} );
                 block.Storage( SurfaceCacheIndirectLightingShaderParameters::SurfaceCardBuffer, buffers.Cards,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheIndirectLightingShaderParameters::SurfaceCardPageBuffer, buffers.Pages,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheIndirectLightingShaderParameters::SurfaceLightingPages,
                                buffers.LightingPages, RDG::Access::StorageRead )
                      .Storage( SurfaceCacheIndirectLightingShaderParameters::RadianceCacheParamsUB, inputs.Params,
                                RDG::Access::StorageRead )
                      .Storage( SurfaceCacheIndirectLightingShaderParameters::RadianceCacheIndirectionUB,
                                inputs.Indirection, RDG::Access::StorageRead )
                      .Storage( SurfaceCacheIndirectLightingShaderParameters::RadianceCacheProbesUB, inputs.Probes,
                                RDG::Access::StorageRead )
                      .Texture( SurfaceCacheIndirectLightingShaderParameters::RadianceCacheIrradianceTexture,
                                inputs.Irradiance, RDG::Access::SampledGraphics, all )
                      .Sampler( SurfaceCacheIndirectLightingShaderParameters::LinearClampSampler,
                                RDG::SamplerDesc::LinearClamp() )
                      .Texture( SurfaceCacheIndirectLightingShaderParameters::RadianceCacheDepthTexture,
                                inputs.Depth, RDG::Access::SampledGraphics, all )
                      .Texture( SurfaceCacheIndirectLightingShaderParameters::AtlasDirectLightingTexture, direct,
                                RDG::Access::SampledGraphics, all )
                      .Sampler( SurfaceCacheIndirectLightingShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( SurfaceCacheIndirectLightingShaderParameters::RadiosityTraceTexture, trace,
                                RDG::Access::SampledGraphics, all );
                 for ( std::size_t i = 0; i < layers.size(); ++i )
                     block.Texture( kLayerTextures[i], layers[i], RDG::Access::SampledGraphics, all )
                          .Sampler( SurfaceCacheIndirectLightingShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() );
                 pass.ColorTarget( 0, indirect, RDG::LoadOp::Load() );
                 pass.ColorTarget( 1, finalLit, RDG::LoadOp::Load() );
             },
             [raw, count]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 // Six vertices per relit page (its atlas rectangle), one instance per page.
                 return Renderer::DrawProcedural( bindings, *raw, nullptr, 6u, count );
             } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::SurfaceCache

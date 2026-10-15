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

namespace Desert::Graphic::SurfaceCache
{
    namespace
    {
        constexpr std::array<std::string_view, 5> kLayerNames{ "SurfaceCache.Depth", "SurfaceCache.Albedo",
                                                               "SurfaceCache.Opacity", "SurfaceCache.Normal",
                                                               "SurfaceCache.Emissive" };
        // The shader names of the atlas layers, in kCapturedLayers order (Programs/SurfaceCache/*.shader).
        constexpr std::array<std::string_view, 5> kLayerImages{ "u_AtlasDepth", "u_AtlasAlbedo", "u_AtlasOpacity",
                                                                "u_AtlasNormal", "u_AtlasEmissive" };
        constexpr uint32_t kCopyGroup = 8; // Programs/SurfaceCache/SurfaceCacheCardCopy.shader LocalSize

        constexpr std::array<std::string_view, 2> kLitLayerNames{ "SurfaceCache.DirectLighting",
                                                                  "SurfaceCache.FinalLighting" };

        // Programs/SurfaceCache/SurfaceCacheDirectLighting.shader SurfaceDirectLightingPush.
        struct DirectLightingPush
        {
            glm::vec4                                                          LightDir{ 0.0f };
            glm::vec4                                                          LightColor{ 0.0f };
            std::array<glm::vec4, DistanceField::kGlobalDistanceFieldClipmaps> GdfLevels{}; // (centre, voxel) cm
        };
        static_assert( sizeof( DirectLightingPush ) == 96 );

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
        m_LightingShader   = shaders->GetByName( "SurfaceCacheDirectLighting" );
        for ( const auto& [shader, name] : { std::pair{ m_CopyShader.get(), "SurfaceCacheCardCopy" },
                                             std::pair{ m_AtlasViewShader.get(), "SurfaceCacheAtlasView" },
                                             std::pair{ m_SceneViewShader.get(), "SurfaceCacheSceneView" },
                                             std::pair{ m_LightingShader.get(), "SurfaceCacheDirectLighting" } } )
            if ( shader == nullptr )
                return Common::MakeFormattedError<bool>( "surface cache: shader {} not found", name );
        auto pipeline = ComputePipeline::Create( { .Shader = m_CopyShader, .DebugName = "SurfaceCacheCardCopy" } );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        m_CopyPipeline = pipeline.GetValue();

        // The direct lighting: one quad per relit page into the two lit layers (no depth, no blend: every texel of
        // a relit page is written).
        GraphicsPipelineSpecification lighting;
        lighting.DebugName = "SurfaceCacheDirectLighting";
        lighting.TargetLayout =
             RenderTargetLayout{ .ColorFormats = { kLayerFormats[static_cast<std::size_t>( kLitLayers[0] )],
                                                   kLayerFormats[static_cast<std::size_t>( kLitLayers[1] )] },
                                 .DepthFormat  = std::nullopt,
                                 .Samples      = 1 };
        lighting.Shader             = m_LightingShader;
        lighting.DepthTestEnabled   = false;
        lighting.DepthWriteEnabled  = false;
        const auto lightingPipeline = GraphicsPipeline::Create( lighting );
        if ( !lightingPipeline )
            return Common::MakeError<bool>( lightingPipeline.GetError() );
        m_LightingPipeline = lightingPipeline.GetValue();

        // The capture atlas: the RSM's slots (the light-view permutation's outputs), its own fixed side.
        FramebufferSpecification spec;
        spec.DebugName = "SurfaceCacheCapture";
        // The G-buffer's own attachments (the view-pass cell's pipeline is built against GBufferLayout); the
        // velocity slot is the capture pass's graph transient (SceneRenderer::AddFrameSurfaceCacheView).
        for ( const Core::Formats::ImageFormat format :
              { ViewTargetFormats::kGBufferA, ViewTargetFormats::kGBufferB, ViewTargetFormats::kGBufferShadingWord,
                ViewTargetFormats::kGBufferEmissive } )
            spec.Attachments.Attachments.emplace_back( format );
        spec.Attachments.Attachments.emplace_back( ViewTargetFormats::kGBufferDepth );
        m_CaptureBuffer = Framebuffer::Create( spec );
        if ( !m_CaptureBuffer )
            return Common::MakeError<bool>( "surface cache: the capture framebuffer could not be created" );
        if ( const auto resized = m_CaptureBuffer->Resize( kCaptureAtlasSize, kCaptureAtlasSize ); !resized )
            return Common::MakeFormattedError<bool>( "surface cache: capture atlas {}²: {}", kCaptureAtlasSize,
                                                     resized.GetError() );
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
        m_AtlasSize = budget.AtlasSize;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SurfaceCacheRenderer::AddCopyPass( RDG::Builder& graph, const ImportImage& import,
                                                             const CaptureTextures& capture,
                                                             const SceneFrame&      frame )
    {
        if ( frame.Copies.empty() )
            return BOOLSUCCESS;
        std::array<RDG::TextureRef, 5> layers;
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
                 block.Sampled( "u_CaptureAlbedo", capture.Albedo, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_CaptureNormal", capture.Normal, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_CaptureEmissive", capture.Emissive, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_CaptureDepth", capture.Depth, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Storage( "SurfaceCardPageCopies", copies, RDG::Access::StorageRead );
                 for ( std::size_t i = 0; i < layers.size(); ++i )
                     block.Storage( kLayerImages[i], layers[i], RDG::Access::StorageWrite );
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
        std::array<RDG::TextureRef, 5> layers;
        for ( std::size_t i = 0; i < layers.size(); ++i )
            layers[i] = import( m_Layers[i], kLayerNames[i] );
        GraphicsPipeline* const raw = pipeline.GetValue().get();
        graph.AddPass(
             "Debug: Surface Cache Atlas", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 auto block = pass.Bindings( m_AtlasViewLayout.Get( m_AtlasViewShader ), RDG::OtherRouteFill{} );
                 for ( std::size_t i = 0; i < layers.size(); ++i )
                     if ( kCapturedLayers[i] != Layer::Emissive )
                         block.Sampled( kLayerImages[i], layers[i], RDG::Access::SampledGraphics,
                                        RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() );
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
        const RDG::TextureRef color = lit ? import( m_LitLayers[1], kLitLayerNames[1] ) : albedo;
        SceneViewPush push;
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
                      .Storage( "SurfaceCardBuffer", cards, RDG::Access::StorageRead )
                      .Storage( "SurfaceCardPageBuffer", pages, RDG::Access::StorageRead )
                      .Sampled( "u_AtlasColor", color, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                                RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_AtlasDepth", depthLayer, RDG::Access::SampledGraphics,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_AtlasOpacity", opacity, RDG::Access::SampledGraphics,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
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
        return { .Cards = UploadBuffer( graph, frame.Cards, "SurfaceCache.Cards" ),
                 .Pages = UploadBuffer( graph, frame.Pages, "SurfaceCache.Pages" ) };
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
        for ( const RDG::TextureRef& clipmap : inputs.Field.Clipmaps )
            if ( !clipmap.IsValid() )
                return Common::MakeError<bool>( "surface cache direct lighting: the frame has no global distance "
                                                "field (its shadow rays off the view's shadow maps)" );
        std::array<RDG::TextureRef, 5> layers;
        for ( std::size_t i = 0; i < layers.size(); ++i )
            layers[i] = import( m_Layers[i], kLayerNames[i] );
        const RDG::TextureRef direct   = import( m_LitLayers[0], kLitLayerNames[0] );
        const RDG::TextureRef finalLit = import( m_LitLayers[1], kLitLayerNames[1] );
        const RDG::BufferRef  relit    = UploadBuffer( graph, frame.LightingPages, "SurfaceCache.LightingPages" );
        const RDG::BufferRef shadow = UploadBuffer( graph, std::vector{ inputs.Shadow }, "SurfaceCache.ShadowUB" );
        const RDG::BufferRef cloud =
             UploadBuffer( graph, std::vector{ inputs.Cloud }, "SurfaceCache.CloudShadowUB" );

        DirectLightingPush push;
        push.LightDir                 = inputs.LightDir;
        push.LightColor               = inputs.LightColor;
        for ( int level = 0; level < DistanceField::kGlobalDistanceFieldClipmaps; ++level )
            push.GdfLevels[level] = inputs.Field.Levels[level];
        GraphicsPipeline* const raw   = m_LightingPipeline.get();
        const uint32_t          count = static_cast<uint32_t>( frame.LightingPages.size() );
        graph.AddPass(
             std::format( "SurfaceCache: Direct Lighting ({} pages)", count ), RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 const auto& layout = m_LightingLayout.Get( m_LightingShader );
                 auto        block  = pass.Bindings( layout, RDG::OtherRouteFill{} );
                 block.Storage( "SurfaceCardBuffer", buffers.Cards, RDG::Access::StorageRead )
                      .Storage( "SurfaceCardPageBuffer", buffers.Pages, RDG::Access::StorageRead )
                      .Storage( "SurfaceLightingPages", relit, RDG::Access::StorageRead )
                      .Storage( ShaderProtocols::PointLight::Name, inputs.Lights.Point, RDG::Access::StorageRead )
                      .Storage( ShaderProtocols::SpotLight::Name, inputs.Lights.Spot, RDG::Access::StorageRead )
                      .Storage( System::kLocalShadowViewsBlockName, inputs.Lights.ShadowViews,
                                RDG::Access::StorageRead )
                      .Storage( "SurfaceCacheShadowUB", shadow, RDG::Access::StorageRead )
                      .Storage( "SurfaceCacheCloudShadowUB", cloud, RDG::Access::StorageRead )
                      .Storage( kLightGridParamsBlockName, inputs.Lights.WorldGridParams,
                                RDG::Access::StorageRead )
                      .Storage( kLightGridWorldCellsBlockName, inputs.Lights.WorldGridCells,
                                RDG::Access::StorageRead )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( DirectLightingPush ) ) );
                 for ( std::size_t i = 0; i < layers.size(); ++i )
                     block.Sampled( kLayerImages[i], layers[i], RDG::Access::SampledGraphics,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() );
                 // The global distance field, REPEAT-sampled (its clipmaps are addressed toroidally).
                 for ( int level = 0; level < DistanceField::kGlobalDistanceFieldClipmaps; ++level )
                     block.Sampled( std::format( "u_GlobalDistanceField{}", level ), inputs.Field.Clipmaps[level],
                                    RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                                    RDG::SamplerDesc::LinearRepeat() );
                 // The cascades, the cloud map and the local shadow atlas, with the composite's samplers.
                 if ( layout )
                     BindSceneViewInputs( block, inputs.View, *layout );
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
} // namespace Desert::Graphic::SurfaceCache

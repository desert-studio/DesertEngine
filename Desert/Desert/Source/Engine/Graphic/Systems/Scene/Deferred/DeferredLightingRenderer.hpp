#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/DeferredTileLists.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ShaderProtocols/LightPayloadBytes.hpp>
#include <Engine/Graphic/LocalShadows.hpp>
#include <Engine/Graphic/LightGrid.hpp>
#include <Engine/Graphic/RenderMethods.hpp>

#include <Common/Core/Profiler.hpp>

#include <glm/glm.hpp>

#include <Common/Core/Logger.hpp>

#include <array>
#include <bit>
#include <format>
#include <vector>
#include <map>
#include <utility>
#include <span>
#include <string>
#include <type_traits>
#include <string_view>

namespace Desert::Graphic::System
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct DeferredLightingWorldViewShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, DeviceDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridWorldViewUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct DeferredLightingCompositeShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWordTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferEmissiveTexture );
        DESERT_SHADER_PARAMETER( Texture, SsaoTexture );
        DESERT_SHADER_PARAMETER( Texture, SsrTexture );
        DESERT_SHADER_PARAMETER( Texture, GiDiffuseTexture );
        DESERT_SHADER_PARAMETER( Texture, GiSpecularTexture );
        DESERT_SHADER_PARAMETER( Texture, MegaLightsDiffuseTexture );
        DESERT_SHADER_PARAMETER( Texture, MegaLightsSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SubsurfaceDiffuseTexture );
        DESERT_SHADER_PARAMETER( Texture, ScreenShadowMaskTexture );
        DESERT_SHADER_PARAMETER( Texture, LocalShadowMasksTexture );
        DESERT_SHADER_PARAMETER( Texture, GiBackfaceTexture );
        // "Deferred: Direct Lighting" -> "Deferred: Composite": a scattered texel's direct specular (the direct
        // program's storage image, the composite's texture) and the direct sum (the direct pass's colour target).
        DESERT_SHADER_PARAMETER( StorageTexture, DeferredDirectSpecularImage );
        DESERT_SHADER_PARAMETER( Texture, DeferredDirectSpecularTexture );
        DESERT_SHADER_PARAMETER( Texture, DeferredDirectLightingTexture );
    };

    // "Deferred: Tile Classify" (DeferredTileClassify.shader) and the tile list the tiled permutations of
    // DeferredLighting.shader read in their vertex stage (Common/DeferredTiles.glslh).
    struct DeferredTileShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWordTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, DeferredTilesUB );
    };

    // GPU-TILE-CLASSIFY (UE Substrate ClassificationTiles): how a lighting pass covers the view. Fullscreen: one
    // triangle (RenderPipeline.TileClassify 0, or a G-buffer view). Simple / Complex: the tiles of that list
    // (Common/DeferredTiles.glslh), drawn indirectly; a tile with no geometry is drawn by neither.
    enum class DeferredTileClass : uint8_t
    {
        Fullscreen = 0,
        Simple,
        Complex,
    };

    // The graph textures the deferred composite samples, as refs of THIS frame's graph that the Composite node
    // declared reads of. An input not produced this frame is the system texture neutral for it (UE
    // GSystemTextures): never an unbound slot, never a skipped draw.
    struct DeferredCompositeInputs
    {
        RDG::TextureRef GBufferA;        // albedo + metallic
        RDG::TextureRef GBufferB;        // normal + roughness
        RDG::TextureRef GBufferShadingWord; // the uint shading word (R32_UINT)
        RDG::TextureRef GBufferDepth;       // device depth: world position is reconstructed from it
        RDG::TextureRef GBufferEmissive; // HDR emissive
        RDG::TextureRef SSAO;            // FrameTransients::SSAO, or System.White (AO = 1)
        RDG::TextureRef
             SunShadowMask;  // "Shadows: Ray Traced Sun" (DeferredScreenShadowMask); bound by its program only
        RDG::TextureRef SSR; // SSR's resolved reflection (rgb, a = coverage), or System.Black (the environment's)
        // The traced GI's final gather, per pixel: diffuse irradiance and specular radiance (FrameTransients::
        // GIDiffuse / GISpecular, SceneRenderer::AddFrameTracedGI), or System.Black (unread in the other GI
        // modes).
        RDG::TextureRef GIDiffuse;
        RDG::TextureRef GISpecular;
        // MegaLights' denoised, demodulated local lighting, diffuse and specular (System::MegaLightsRenderer), or
        // System.Black; bound by the MegaLights program only (DeferredShadowInput::MegaLightsLocalLights).
        // "Shadows: Ray Traced Local" (a 2D array, one layer per ShadowMaskSlot); bound by its program only.
        RDG::TextureRef LocalShadowMasks;
        RDG::TextureRef MegaLightsDiffuse;
        RDG::TextureRef MegaLightsSpecular;
        RDG::TextureRef GIBackface; // irradiance from -N, the light through a two-sided leaf ("GiBackfaceTexture")
        // SSS-PROFILE (CONTRACT §4): the demodulated diffuse of the screen-space scattered texels, written when the
        // view scatters (DeferredGIInput::SubsurfaceSplit); a 1x1 transient nothing writes otherwise.
        RDG::TextureRef SubsurfaceDiffuse;
        // The direct sum (ViewTargetFormats::kDeferredDirectLighting, the scene target's extent) and a scattered
        // texel's direct specular (kDeferredDirectSpecular; a 1x1 transient nothing reads when the view does
        // not scatter): written by "Deferred: Direct Lighting", read by "Deferred: Composite".
        RDG::TextureRef DirectLighting;
        RDG::TextureRef DirectSpecular;
        // The scene/view inputs (SceneViewInputsOf): cascades, environment cubes, BRDF LUT, cloud shadow map.
        SceneViewInputs View;
    };

    // The view's lights as graph buffers (ViewLightBuffers, uploaded by the graph's upload command before the
    // nodes that read them) plus the CPU copy of the grid's parameters: the dispatch size of "LightGrid".
    struct DeferredCompositeLights : ViewLightBuffers
    {
        LightGridParamsPayload Grid;
    };

    // Deferred lighting + G-buffer debug pass. Fullscreen: reads the scene renderer's MRT G-buffer and writes
    // the shaded (or debug) result into the scene target framebuffer, which the post chain then tonemaps.
    // Runs in the manual chain (like Tonemap), only when RenderPath == Deferred.
    class DeferredLightingRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // The permutation of the direct-lighting program @p program (kCascadeProgram..kMegaLightsCascadeProgram;
        // each carries kDirectDefine, the shader without defines is the composite), cooked: ShippedByCook - the one
        // statement BuildProgram acquires and ProgramVariants lists.
        [[nodiscard]] static Core::ShaderVariant ProgramVariant( const std::size_t program )
        {
            Core::ShaderVariant variant;
            variant.Defines.emplace_back( kDirectDefine );
            // Every permutation but the cascades' own and MegaLights under StochasticVisibility::Screen reads
            // the sun's screen mask; those two keep the sun's cascades (the screen source draws no sun mask).
            if ( program != kCascadeProgram && program != kMegaLightsCascadeProgram )
                variant.Defines.emplace_back( kSunShadowScreenMaskDefine );
            if ( program == kMegaLightsProgram || program == kMegaLightsCascadeProgram )
                variant.Defines.emplace_back( kMegaLightsDefine );
            if ( program == kRayTracedLocalProgram || program == kVirtualLocalProgram )
                variant.Defines.emplace_back( kLocalShadowScreenMaskDefine );
            // Shadows VirtualMaps: the same masks by slot, and the virtual map itself (no atlas) for what the
            // masks do not hold - the subsurface transmission (DeferredLighting.shader DESERT_LOCAL_SHADOW_VSM).
            if ( program == kVirtualLocalProgram )
                variant.Defines.emplace_back( kLocalShadowVsmDefine );
            variant.ShippedByCook = true;
            return variant;
        }

        // Every permutation the renderer acquires, cooked by the package (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants()
        {
            std::vector<Core::CookedProgramVariant> variants;
            for ( std::size_t index = kCascadeProgram; index < kProgramDebugNames.size(); ++index )
            {
                variants.push_back( { "DeferredLighting", ProgramVariant( index ) } );
                for ( std::size_t tiled = 0; tiled < kTiledClassCount; ++tiled )
                    variants.push_back( { "DeferredLighting", TileVariant( ProgramVariant( index ), tiled ) } );
                for ( const uint32_t model : TileModels() )
                    variants.push_back( { "DeferredLighting", ModelVariant( ProgramVariant( index ), model ) } );
            }
            for ( std::size_t tiled = 0; tiled < kTiledClassCount; ++tiled )
                variants.push_back( { "DeferredLighting", TileVariant( Core::ShaderVariant{}, tiled ) } );
            return variants;
        }

        // FPS-DIRECT-TILES (UE ClusteredDeferredShading's per-shading-model tile types): the direct program
        // @p variant drawn as the tiles of model @p model's own list - every geometry texel of such a tile is
        // model index @p model, so DeferredLighting.shader folds the model to that constant
        // (DESERT_DEFERRED_TILE_MODEL) and the program holds only that model's functions. Cooked.
        [[nodiscard]] static Core::ShaderVariant ModelVariant( Core::ShaderVariant variant, const uint32_t model )
        {
            variant.Defines.emplace_back( kTilesDefine );
            variant.Defines.emplace_back( std::format( "{}={}", kTileModelDefine, model ) );
            variant.ShippedByCook = true;
            return variant;
        }

        // The shading-model indices that get a list and a folded permutation of their own: every model of the
        // loaded set (ShaderRootShadingModels) but DefaultLit, whose tiles are the SIMPLE list already. Empty when
        // the set did not load - said here; every complex tile is then a mixed one (the same picture).
        [[nodiscard]] static std::vector<uint32_t> TileModels()
        {
            std::vector<uint32_t>                    models;
            const Core::ShadingModels::ShaderRootSet held = Core::ShadingModels::ShaderRootShadingModels();
            if ( !held->IsSuccess() )
            {
                LOG_ERROR( "Deferred lighting: the shading models did not load ({}): no per-model tile lists",
                           held->GetError() );
                return models;
            }
            for ( const Core::ShadingModels::ShadingModelEntry& entry : held->GetValue().Registry.Entries() )
                if ( entry.Manifest.Name != kTileSimpleModel && entry.Index < DeferredTileLists::kMaxModelLists )
                    models.push_back( entry.Index );
            return models;
        }

        // @p variant drawn as the tiles of list @p tiled (kSimpleTiles / kComplexTiles; DeferredLighting.shader
        // DESERT_DEFERRED_TILES, and DESERT_DEFERRED_TILE_SIMPLE for the DefaultLit-only list), cooked.
        [[nodiscard]] static Core::ShaderVariant TileVariant( Core::ShaderVariant variant, const std::size_t tiled )
        {
            variant.Defines.emplace_back( kTilesDefine );
            if ( tiled == kSimpleTiles )
                variant.Defines.emplace_back( kTileSimpleDefine );
            variant.ShippedByCook = true;
            return variant;
        }

        virtual Common::BoolResultStr Initialize() override
        {
            // The shader without defines is the composite; every direct program is a permutation of it.
            m_Composite.Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DeferredLighting" );
            if ( !m_Composite.Shader )
                return Common::MakeError( "DeferredLighting shader not found" );
            if ( const Common::BoolResultStr built = BuildComposite(); !built )
                return Common::MakeError( std::format( "Deferred lighting: {}", built.GetError() ) );
            m_Refusals = {};
            for ( std::size_t index = 0; index < m_Programs.size(); ++index )
            {
                const Common::BoolResultStr built = BuildProgram( index );
                if ( built )
                    continue;
                // The cascades' program draws CascadedMaps, the ladder's front: without it there is no deferred
                // composite at all. Any other program serves only the Shadows methods it is named for: those are
                // refused by name (ResolveRenderMethods walks on down the ladder), the composite keeps the rest.
                if ( kProgramShadowMethods[index].empty() )
                    return Common::MakeError( std::format( "Deferred lighting: {}", built.GetError() ) );
                RefuseProgram( index, built.GetError() );
            }

            m_GridShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "LightGridBuild" );
            if ( !m_GridShader )
                return Common::MakeError( "LightGridBuild shader not found" );
            const auto gridPipeline =
                 ComputePipeline::Create( { .Shader = m_GridShader, .DebugName = "LightGridBuild" } );
            if ( !gridPipeline )
                return Common::MakeError( gridPipeline.GetError() );
            m_GridPipeline = gridPipeline.GetValue();

            m_WorldViewShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "LightGridWorldView" );
            if ( !m_WorldViewShader )
                return Common::MakeError( "LightGridWorldView shader not found" );

            m_ClassifyShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DeferredTileClassify" );
            if ( !m_ClassifyShader )
                return Common::MakeError( "DeferredTileClassify shader not found" );
            const auto classifyPipeline =
                 ComputePipeline::Create( { .Shader = m_ClassifyShader, .DebugName = "DeferredTileClassify" } );
            if ( !classifyPipeline )
                return Common::MakeError( classifyPipeline.GetError() );
            m_ClassifyPipeline = classifyPipeline.GetValue();
            BuildTiledPrograms();
            return BOOLSUCCESS;
        }

        // True when "Deferred: Tile Classify" can run (its pipeline built at Initialize).
        [[nodiscard]] bool ClassifyReady() const noexcept
        {
            return m_ClassifyPipeline != nullptr;
        }

        // True when this frame's direct program (the arm of @p shadow) and the composite have both tiled
        // permutations built and the classify is ready: the view may draw by tiles (RenderPipeline.TileClassify).
        // A permutation that did not build was said at its build (BuildTiledPrograms), with its reason.
        [[nodiscard]] bool TileClassifyReady( const DeferredShadowInput& shadow ) const
        {
            if ( !m_ClassifyPipeline )
                return false;
            const std::size_t program = ActiveProgramOf( shadow );
            for ( std::size_t tiled = 0; tiled < kTiledClassCount; ++tiled )
                if ( !m_TiledPrograms[program][tiled].Pipeline || !m_TiledComposite[tiled].Pipeline )
                    return false;
            return true;
        }

        // GRAPH BUILD (before "Deferred: Tile Classify"): the tile lists of a @p width x @p height view (the
        // lighting targets' extent), their header uploaded - both draw records empty, the complex one's first
        // vertex past the simple list, and the extent the classify and the vertex stage read.
        // FPS-DIRECT-TILES: then the mixed tiles' record, the mask of the models with a list of their own
        // (ModelListMask) and one record per model index, each list's first vertex past the lists before it.
        DeferredTileLists CreateTileLists( RDG::Builder& graph, const uint32_t width, const uint32_t height ) const
        {
            DeferredTileLists lists;
            lists.Tiles          = glm::uvec2( TileGrid( width ), TileGrid( height ) );
            lists.Extent         = glm::uvec2( width, height );
            const uint32_t count = lists.Tiles.x * lists.Tiles.y;
            // The lists up to the mask's highest model: a model index past it is never listed (the classify tests
            // the mask), so its list takes no memory.
            const uint32_t modelLists =
                 m_ModelListMask == 0u ? 0u : 32u - static_cast<uint32_t>( std::countl_zero( m_ModelListMask ) );
            const uint64_t bytes =
                 ( uint64_t{ kTileHeaderWords } +
                   uint64_t{ DeferredTileLists::kListsBeforeModels + modelLists } * uint64_t{ count } ) *
                 sizeof( uint32_t );
            lists.Buffer         = graph.CreateBuffer( RDG::BufferDesc{ bytes }, "DeferredTiles" );
            // Both draw records, the extent, then both dispatch records (Common/DeferredTiles.glslh).
            std::array<uint32_t, kTileHeaderWords> header = {
                 0u, 1u, 0u, 0u, 0u, 1u, count * kTileVertices, 0u, width, height, lists.Tiles.x, count, 0u, 1u,
                 1u, 0u, 0u, 1u, 1u, 0u, 0u, 1u, 2u * count * kTileVertices, 0u, m_ModelListMask, 0u, 0u, 0u };
            for ( uint32_t model = 0; model < DeferredTileLists::kMaxModelLists; ++model )
            {
                const uint32_t word = DeferredTileLists::kModelDrawWord + 4u * model;
                header[word + 1u]   = 1u;
                header[word + 2u]   = ( DeferredTileLists::kListsBeforeModels + model ) * count * kTileVertices;
            }
            graph.QueueBufferUpload( lists.Buffer, std::as_bytes( std::span<const uint32_t>( header ) ) );
            return lists;
        }

        // SETUP of "Deferred: Tile Classify": the G-buffer normal and shading word read, the lists written.
        void DeclareClassifyBindings( RDG::PassBuilder& pass, const RDG::TextureRef normal,
                                      const RDG::TextureRef shadingWord, const RDG::BufferRef tiles ) const
        {
            if ( !m_ClassifyPipeline )
                return;
            pass.Bindings( m_ClassifyLayout.Get( m_ClassifyShader ),
                           Renderer::GetPipelineRouteFill( *m_ClassifyPipeline ) )
                 .Sampler( DeferredTileShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( DeferredTileShaderParameters::GBufferBTexture, normal, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Texture( DeferredTileShaderParameters::GBufferShadingWordTexture, shadingWord,
                           RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                 .Storage( DeferredTileShaderParameters::DeferredTilesUB, tiles, RDG::Access::StorageWrite );
        }

        // EXEC of the same node: one workgroup per tile (DeferredTileClassify.shader LocalSize 8 x 8).
        [[nodiscard]] Common::BoolResultStr RecordClassify( const RDG::PassContext& context,
                                                            const glm::uvec2&       tiles ) const
        {
            if ( !m_ClassifyPipeline )
                return Common::MakeError( "Deferred: Tile Classify: the classify pipeline is not initialised" );
            DESERT_PROFILE_PASS( "Deferred: Tile Classify" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *m_ClassifyPipeline, tiles.x, tiles.y, 1 );
        }

        // The Shadows methods this renderer could not build a program for on this device (Initialize), which
        // SceneRenderer hands to ResolveRenderMethods so the view renders the next method down instead.
        [[nodiscard]] const ProgramRefusals& Refusals() const
        {
            return m_Refusals;
        }

        // The programs refused at Initialize (or at the last retry) are acquired and built again - the shader
        // service recompiled a program since (SceneRenderer::RetryRefusedProgramsAfterRecompile), so new bytes may
        // build. Refusals() then holds exactly the programs that still do not, each with its current reason; the
        // programs that built are left as they are. Says nothing: the scene renderer reports each refusal once.
        void RetryRefusedPrograms()
        {
            m_Refusals = {};
            for ( std::size_t index = kScreenMaskSunProgram; index < m_Programs.size(); ++index )
            {
                if ( m_Programs[index].Pipeline )
                    continue;
                if ( const Common::BoolResultStr built = BuildProgram( index ); !built )
                    RefuseProgram( index, built.GetError() );
            }
            BuildTiledPrograms();
        }

        // The "Light Grid (World)" view mode's pipeline for a target of @p format at @p samples (the view's post
        // input), made the first time it is asked for.
        [[nodiscard]] Common::BoolResultStr PrepareWorldView( const Core::Formats::ImageFormat format,
                                                              const uint32_t                   samples )
        {
            if ( !m_WorldViewShader )
                return Common::MakeError( "Debug: LightGridWorld: the LightGridWorldView shader is not loaded" );
            const auto key = std::make_pair( static_cast<int>( format ), samples );
            if ( const auto it = m_WorldViewPipelines.find( key ); it != m_WorldViewPipelines.end() )
            {
                m_WorldView = it->second;
                return BOOLSUCCESS;
            }
            GraphicsPipelineSpecification spec;
            spec.DebugName = "LightGridWorldView";
            spec.TargetLayout =
                 RenderTargetLayout{ .ColorFormats = { format }, .DepthFormat = std::nullopt, .Samples = samples };
            spec.Shader            = m_WorldViewShader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_WorldView = m_WorldViewPipelines.emplace( key, pipeline.GetValue() ).first->second;
            return BOOLSUCCESS;
        }

        // SETUP of "Debug: LightGridWorld": the G-buffer depth, the inverse view-projection it was rasterised with
        // (@p view, an uploaded mat4), the world grid's parameters and cells.
        void DeclareWorldViewBindings( RDG::PassBuilder& pass, RDG::TextureRef depth, RDG::BufferRef view,
                                       const ViewLightBuffers& lights ) const
        {
            if ( !m_WorldView )
                return;
            pass.Bindings( m_WorldViewLayout.Get( m_WorldViewShader ), RDG::NoOtherRouteFill() )
                 .Texture( DeferredLightingWorldViewShaderParameters::DeviceDepthTexture, depth,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Sampler( DeferredLightingWorldViewShaderParameters::LinearRepeatSampler,
                           RDG::SamplerDesc::LinearRepeat() )
                 .Storage( DeferredLightingWorldViewShaderParameters::LightGridWorldViewUB, view,
                           RDG::Access::StorageRead )
                 .Storage( kLightGridParamsBlock, lights.WorldGridParams, RDG::Access::StorageRead )
                 .Storage( kLightGridWorldCellsBlock, lights.WorldGridCells, RDG::Access::StorageRead );
        }

        [[nodiscard]] Common::BoolResultStr RecordWorldView( const RDG::PassContext& context ) const
        {
            if ( !m_WorldView )
                return Common::MakeError( "Debug: LightGridWorld: no pipeline was prepared for the target" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_WorldView, nullptr );
        }

        // Not a render-graph pass — driven from SceneRenderer's manual chain after the geometry graph.

        // GRAPH BUILD (before the Composite node): the lights as graph buffers, each uploaded by the graph's
        // upload command. No light is one zeroed entry (a storage buffer is never empty); the counts are the
        // material's LightsMetadata. @p grid = MakeLightGridParams of this view: its parameters are uploaded too
        // and its cell buffer created, which "LightGrid" fills.
        static DeferredCompositeLights UploadLights( RDG::Builder&                              graph,
                                                     const ShaderProtocols::PointLight&         points,
                                                     const ShaderProtocols::SpotLight&          spots,
                                                     const std::vector<LocalShadowViewPayload>& shadowViews,
                                                     const LightGridParamsPayload&              grid )
        {
            const auto upload = [&graph]( const auto& lights, std::string_view name )
            {
                const std::span<const std::byte> bytes  = ShaderProtocols::LightPayloadBytes( lights );
                const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ bytes.size() }, name );
                graph.QueueBufferUpload( buffer, bytes );
                return buffer;
            };
            const RDG::BufferRef gridParams =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( LightGridParamsPayload ) }, "Lights.GridParams" );
            graph.QueueBufferUpload( gridParams,
                                     std::as_bytes( std::span<const LightGridParamsPayload>( &grid, 1 ) ) );
            const RDG::BufferRef gridCells =
                 graph.CreateBuffer( RDG::BufferDesc{ LightGridCellBufferBytes( grid ) }, "Lights.GridCells" );
            DeferredCompositeLights lights;
            lights.Point       = upload( points.PointLights, "Lights.Point" );
            lights.Spot        = upload( spots.SpotLights, "Lights.Spot" );
            lights.ShadowViews = upload( shadowViews, "Lights.ShadowViews" );
            lights.GridParams  = gridParams;
            lights.GridCells   = gridCells;
            lights.Grid        = grid;
            return lights;
        }

        // GRAPH BUILD (before "LightGridWorld"): the world layout's parameters uploaded and its cell buffer
        // created, its pool allocator (the buffer's first uint) zeroed by an upload, into @p lights' World* refs.
        // @p grid = MakeLightGridWorldParams of this view. The point and spot buffers are the ones UploadLights
        // made: the world layout lists the SAME lights, there is no second light list.
        static void UploadWorldGrid( RDG::Builder& graph, const LightGridParamsPayload& grid,
                                     ViewLightBuffers& lights )
        {
            lights.WorldGridParams = graph.CreateBuffer( RDG::BufferDesc{ sizeof( LightGridParamsPayload ) },
                                                         "Lights.WorldGridParams" );
            graph.QueueBufferUpload( lights.WorldGridParams,
                                     std::as_bytes( std::span<const LightGridParamsPayload>( &grid, 1 ) ) );
            lights.WorldGridCells    = graph.CreateBuffer( RDG::BufferDesc{ LightGridCellBufferBytes( grid ) },
                                                           "Lights.WorldGridCells" );
            constexpr uint32_t kZero = 0u;
            graph.QueueBufferUpload( lights.WorldGridCells,
                                     std::as_bytes( std::span<const uint32_t>( &kZero, 1 ) ) );
        }

        // SETUP of a light-grid compute node ("LightGrid" for the frustum layout, "LightGridWorld" for the world
        // one): its one block — the layout's grid parameters and both light buffers read, its cell buffer written.
        void DeclareGridBindings( RDG::PassBuilder& pass, const ViewLightBuffers& lights,
                                  LightGridLayout layout ) const
        {
            if ( !m_GridPipeline )
                return;
            const bool world = layout == LightGridLayout::WorldClipmaps;
            pass.Bindings( m_GridLayout.Get( m_GridShader ), Renderer::GetPipelineRouteFill( *m_GridPipeline ) )
                 .Storage( kLightGridParamsBlock, world ? lights.WorldGridParams : lights.GridParams,
                           RDG::Access::StorageRead )
                 .Storage( ShaderProtocols::PointLight::Parameter, lights.Point, RDG::Access::StorageRead )
                 .Storage( ShaderProtocols::SpotLight::Parameter, lights.Spot, RDG::Access::StorageRead )
                 .Storage( kLightGridCellsBlock, world ? lights.WorldGridCells : lights.GridCells,
                           RDG::Access::StorageWrite );
        }

        // EXEC of the same node: one thread per cell (LightGridBuild.shader's LocalSize 4 x 4 x 4).
        [[nodiscard]] Common::BoolResultStr RecordGrid( const RDG::PassContext& context, const glm::uvec3& cells )
        {
            if ( !m_GridPipeline )
                return Common::MakeError( "LightGrid: the light-grid pipeline is not initialised" );
            constexpr uint32_t      kGroup = 4u;
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DispatchCompute(
                 bindings, *m_GridPipeline, ( cells.x + kGroup - 1u ) / kGroup, ( cells.y + kGroup - 1u ) / kGroup,
                 ( cells.z + kGroup - 1u ) / kGroup );
        }

        // SETUP of "Deferred: Composite", FIRST: the material's values for this frame (lead decision B - a
        // material whose route fill the setup validates is filled before the validation reads it, never in the
        // exec). lightDir.xyz = the direction the sun travels; lightColor.rgb/.a = colour/intensity; cameraPos.xyz
        // = camera world position; debugMode selects a raw channel (0 = lit); gi.Mode picks the indirect-light
        // source (0 = off, 1 = screen-space, 2 = the traced final gather under SSAO, 3 = the traced final gather
        // occluded by its own short-range AO; 2 and 3 read GIDiffuse / GISpecular / GIBackface).
        void FillMaterial( const glm::vec4& lightDir, const glm::vec4& lightColor, const glm::vec4& light1Dir,
                           const glm::vec4& light1Color, const glm::vec4& cameraPos,
                           const glm::mat4& invJitteredViewProjection, int debugMode, uint32_t pointCount,
                           uint32_t spotCount, const DeferredShadowInput& shadow, const DeferredGIInput& gi,
                           bool ssaoEnabled, const CloudShadowInput& cloudShadow,
                           const DeferredEnvironmentInput& environment, const glm::mat4& jitteredViewProjection,
                           const bool tiles )
        {
            m_Active = ActiveProgramOf( shadow );
            // One frame's values for every pass that draws: the direct program this arm picked and the composite
            // (the environment's reader) - their two tiled permutations when @p tiles (the caller asked
            // TileClassifyReady), else the fullscreen ones.
            if ( m_Composite.Material )
                ReportEnvironmentGap( environment );
            std::array<CompositeProgram*, 2 * kTiledClassCount> programs{};
            if ( tiles )
                programs = { &m_TiledPrograms[m_Active][kSimpleTiles], &m_TiledPrograms[m_Active][kComplexTiles],
                             &m_TiledComposite[kSimpleTiles], &m_TiledComposite[kComplexTiles] };
            else
                programs = { &m_Programs[m_Active], &m_Composite, nullptr, nullptr };
            for ( const CompositeProgram* program : programs )
            {
                if ( program == nullptr || !program->Material )
                    continue;
                program->Material->BindInputs( lightDir, lightColor, light1Dir, light1Color, cameraPos,
                                               invJitteredViewProjection, debugMode, pointCount, spotCount,
                                               shadow, gi, ssaoEnabled, cloudShadow, environment,
                                               jitteredViewProjection );
            }
            if ( !tiles )
                return;
            for ( uint32_t model = 0; model < DeferredTileLists::kMaxModelLists; ++model )
            {
                const CompositeProgram& program = m_ModelPrograms[m_Active][model];
                if ( ( m_ModelListMask & ( 1u << model ) ) == 0u || !program.Material )
                    continue;
                program.Material->BindInputs( lightDir, lightColor, light1Dir, light1Color, cameraPos,
                                              invJitteredViewProjection, debugMode, pointCount, spotCount, shadow,
                                              gi, ssaoEnabled, cloudShadow, environment, jitteredViewProjection );
            }
        }

        // SETUP of "Deferred: Direct Lighting", after FillMaterial (GPU-TURN-3; UE RenderLights): the direct
        // program's one block (block 0) - the G-buffer, the light rows or MegaLights' lighting, the sun's and the
        // local lights' shadow inputs and the scene/view inputs its layout declares (cascades, cloud shadow, light
        // grid) - and its colour target, inputs.DirectLighting. UE's shared-sampler form: every input is a
        // separate texture read through the three sampler states the pass binds once. It reads no GI, AO or
        // reflection: nothing the async chain writes, so the graph runs it on the graphics queue inside the async
        // window.
        // @p tileClass: the whole view, or one tile list of @p tiles (the classify's buffer, read by the vertex
        // stage and as the draw's indirect record).
        void DeclareDirectBindings( RDG::PassBuilder& pass, const DeferredCompositeInputs& inputs,
                                    const ViewLightBuffers& lights, const DeferredTileClass tileClass,
                                    const RDG::BufferRef tiles ) const
        {
            const CompositeProgram& program = DirectProgramOf( m_Active, tileClass );
            // RENDER-THREADS M1: the program is chosen ONCE, here; the exec draws this one (the one whose material
            // the graph prepared), whatever m_Active says by then.
            const std::size_t classIndex   = static_cast<std::size_t>( tileClass );
            m_DeclaredPrograms[classIndex] = nullptr;
            if ( !program.Pipeline || !program.Material )
                return;
            m_DeclaredPrograms[classIndex] = &program;
            DeclareDirectBlock( pass, program, inputs, lights, tileClass, tiles );
            // FPS-DIRECT-TILES: the complex tiles' pass draws the mixed list with the dispatch (block 0) and each
            // model's own list with its folded program (blocks 1..), in one render pass.
            m_DeclaredModelCount = 0u;
            if ( tileClass == DeferredTileClass::Complex )
                for ( uint32_t model = 0; model < DeferredTileLists::kMaxModelLists; ++model )
                {
                    const CompositeProgram& folded = m_ModelPrograms[m_Active][model];
                    if ( ( m_ModelListMask & ( 1u << model ) ) == 0u || !folded.Pipeline || !folded.Material )
                        continue;
                    DeclareDirectBlock( pass, folded, inputs, lights, tileClass, tiles );
                    m_DeclaredModels[m_DeclaredModelCount++] = { &folded, model };
                }
            if ( tileClass != DeferredTileClass::Fullscreen )
                pass.Read( tiles, RDG::Access::IndirectArgs );
            // Every geometry texel is written; a sky texel is discarded and never read (the composite returns
            // first). The complex tiles' pass follows the simple tiles' into the same target: it keeps them.
            const RDG::LoadOp load =
                 tileClass == DeferredTileClass::Complex ? RDG::LoadOp::Load() : RDG::LoadOp::DontCare();
            pass.ColorTarget( 0, inputs.DirectLighting, load );
        }

        // One direct program's block of "Deferred: Direct Lighting" (DeclareDirectBindings), in declaration order.
        void DeclareDirectBlock( RDG::PassBuilder& pass, const CompositeProgram& program,
                                 const DeferredCompositeInputs& inputs, const ViewLightBuffers& lights,
                                 const DeferredTileClass tileClass, const RDG::BufferRef tiles ) const
        {
            constexpr RDG::Access           kRead  = RDG::Access::SampledGraphics;
            constexpr RDG::SubresourceRange kWhole = RDG::SubresourceRange::All();
            const SlottedLayout&            slotted = program.Layout.GetSlotted( program.Shader );
            const auto&                     layout  = slotted.Layout;
            pass.Prepares( *program.Material->GetMaterialExecutor() );
            auto block = pass.Bindings( layout, program.Material->GetMaterialExecutor()->GetRouteFill() );
            DeclareGBufferReads( block, inputs, false );
            block.Storage( DeferredLightingCompositeShaderParameters::DeferredDirectSpecularImage,
                           inputs.DirectSpecular, RDG::Access::StorageWrite );
            // The light rows and their shadow views: the light-grid loop's. The MegaLights program reads
            // MegaLights' lighting instead.
            if ( m_Active == kMegaLightsProgram || m_Active == kMegaLightsCascadeProgram )
                block.Texture( DeferredLightingCompositeShaderParameters::MegaLightsDiffuseTexture,
                               inputs.MegaLightsDiffuse, kRead, kWhole )
                     .Texture( DeferredLightingCompositeShaderParameters::MegaLightsSpecularTexture,
                               inputs.MegaLightsSpecular, kRead, kWhole );
            else
                block.Storage( ShaderProtocols::PointLight::Parameter, lights.Point, RDG::Access::StorageRead )
                     .Storage( ShaderProtocols::SpotLight::Parameter, lights.Spot, RDG::Access::StorageRead );
            if ( m_Active == kScreenMaskSunProgram || m_Active == kMegaLightsProgram ||
                 m_Active == kRayTracedLocalProgram || m_Active == kVirtualLocalProgram )
                block.Texture( DeferredLightingCompositeShaderParameters::ScreenShadowMaskTexture,
                               inputs.SunShadowMask, kRead, kWhole );
            if ( m_Active == kRayTracedLocalProgram || m_Active == kVirtualLocalProgram )
                block.Texture( DeferredLightingCompositeShaderParameters::LocalShadowMasksTexture,
                               inputs.LocalShadowMasks, kRead, kWhole );
            if ( layout )
                BindSceneViewInputs( block, inputs.View, slotted.Slots );
            // The indirect-argument read of the buffer is the pass's, once (DeclareDirectBindings).
            if ( tileClass != DeferredTileClass::Fullscreen )
                block.Storage( DeferredTileShaderParameters::DeferredTilesUB, tiles, RDG::Access::StorageRead );
        }

        // SETUP of "Deferred: Composite", after "Deferred: Direct Lighting"'s: the node's one block (block 0) -
        // the G-buffer, the AO, the reflections, the traced GI, the subsurface split's target, the direct pass's
        // two targets and the scene/view inputs the composite's layout declares (the sky's cubes and LUT, the
        // captures, the radiance cache). No light and no shadow: the direct pass summed them (UE
        // DiffuseIndirectComposite).
        void DeclareCompositeBindings( RDG::PassBuilder& pass, const DeferredCompositeInputs& inputs,
                                       const DeferredTileClass tileClass, const RDG::BufferRef tiles ) const
        {
            const CompositeProgram& program    = CompositeProgramOf( tileClass );
            const std::size_t       classIndex = static_cast<std::size_t>( tileClass );
            m_DeclaredComposites[classIndex]   = nullptr;
            if ( !program.Pipeline || !program.Material )
                return;
            m_DeclaredComposites[classIndex]       = &program;
            constexpr RDG::Access           kRead  = RDG::Access::SampledGraphics;
            constexpr RDG::SubresourceRange kWhole = RDG::SubresourceRange::All();
            const SlottedLayout&            slotted = program.Layout.GetSlotted( program.Shader );
            const auto&                     layout  = slotted.Layout;
            pass.Prepares( *program.Material->GetMaterialExecutor() );
            auto block = pass.Bindings( layout, program.Material->GetMaterialExecutor()->GetRouteFill() );
            DeclareGBufferReads( block, inputs, true );
            block.Texture( DeferredLightingCompositeShaderParameters::GBufferEmissiveTexture,
                           inputs.GBufferEmissive, kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::SsaoTexture, inputs.SSAO, kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::SsrTexture, inputs.SSR, kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GiDiffuseTexture, inputs.GIDiffuse, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GiSpecularTexture, inputs.GISpecular, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GiBackfaceTexture, inputs.GIBackface, kRead,
                           kWhole )
                 .Storage( DeferredLightingCompositeShaderParameters::SubsurfaceDiffuseTexture,
                           inputs.SubsurfaceDiffuse, RDG::Access::StorageWrite )
                 .Texture( DeferredLightingCompositeShaderParameters::DeferredDirectLightingTexture,
                           inputs.DirectLighting, kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::DeferredDirectSpecularTexture,
                           inputs.DirectSpecular, kRead, kWhole );
            if ( layout )
                BindSceneViewInputs( block, inputs.View, slotted.Slots );
            if ( tileClass != DeferredTileClass::Fullscreen )
                DeclareTileReads( pass, block, tiles );
        }

        // EXEC of "Deferred: Direct Lighting" (or of one tile list's pass): the direct program
        // DeclareDirectBindings chose, into the direct target.
        [[nodiscard]] Common::BoolResultStr RecordDirect( const RDG::PassContext& context,
                                                          const DeferredTileClass tileClass,
                                                          const RDG::BufferRef    tiles )
        {
            const CompositeProgram* program = m_DeclaredPrograms[static_cast<std::size_t>( tileClass )];
            if ( program == nullptr )
                return Common::MakeError(
                     "Deferred: Direct Lighting: the deferred-lighting pipeline is not initialised" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            if ( tileClass != DeferredTileClass::Complex )
                return DrawClass( bindings, *program, tileClass, tiles );
            if ( const Common::BoolResultStr mixed =
                      Renderer::DrawProceduralIndirect( bindings, *program->Pipeline,
                                                        program->Material->GetMaterialExecutor(), tiles,
                                                        DeferredTileLists::kMixedDrawOffset );
                 !mixed )
                return mixed;
            for ( uint32_t i = 0; i < m_DeclaredModelCount; ++i )
            {
                const DeclaredModel&    declared = m_DeclaredModels[i];
                const RDG::PassBindings folded( context, context.GetBindingBlock( i + 1u ) );
                if ( const Common::BoolResultStr drawn = Renderer::DrawProceduralIndirect(
                          folded, *declared.Program->Pipeline, declared.Program->Material->GetMaterialExecutor(),
                          tiles, DeferredTileLists::ModelDrawOffset( declared.Model ) );
                     !drawn )
                    return drawn;
            }
            return BOOLSUCCESS;
        }

        // EXEC of "Deferred: Composite": shades the G-buffer into the scene target, inside the render pass the
        // frame graph opens on it with LOAD (which preserves the forward-rendered sky/grid; the shader discards
        // non-geometry texels), from block 0 that DeclareCompositeBindings declared.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context,
                                                    const DeferredTileClass tileClass, const RDG::BufferRef tiles )
        {
            const CompositeProgram* program = m_DeclaredComposites[static_cast<std::size_t>( tileClass )];
            if ( program == nullptr )
                return Common::MakeError(
                     "Deferred: Composite: the deferred-lighting pipeline is not initialised" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return DrawClass( bindings, *program, tileClass, tiles );
        }

    private:
        // The arm of @p shadow, the direct program it draws with: the sun's arm, or MegaLights' local lights by
        // the sun's arm (Stochastic's sun follows its visibility source: rayquery the ray-traced mask, vsm+screen
        // the virtual map's mask, screen the cascades).
        [[nodiscard]] static std::size_t ActiveProgramOf( const DeferredShadowInput& shadow )
        {
            const bool sunMask = std::holds_alternative<DeferredScreenShadowMask>( shadow.Sun );
            if ( shadow.MegaLightsLocalLights )
                return sunMask ? kMegaLightsProgram : kMegaLightsCascadeProgram;
            if ( shadow.VirtualLocalLights )
                return kVirtualLocalProgram;
            if ( shadow.RayTracedLocalLights )
                return kRayTracedLocalProgram;
            return shadow.Sun.index();
        }

        // Common/DeferredTiles.glslh: the header's uints, the vertices per tile, the two draw records' bytes.
        static constexpr uint32_t    kTileHeaderWords   = DeferredTileLists::kHeaderWords;
        static constexpr uint32_t    kTileVertices      = DeferredTileLists::kVertices;
        static constexpr uint32_t    kTileSize          = DeferredTileLists::kTileSize;
        static constexpr uint64_t    kSimpleArgsOffset  = DeferredTileLists::kSimpleDrawOffset;
        static constexpr uint64_t    kComplexArgsOffset = DeferredTileLists::kComplexDrawOffset;
        static constexpr std::size_t kSimpleTiles       = 0; // m_TiledPrograms' second index, TileVariant's list
        static constexpr std::size_t kComplexTiles      = 1;
        static constexpr std::size_t kTiledClassCount   = 2;
        static constexpr std::array<std::string_view, kTiledClassCount> kTiledDebugNames = { "SimpleTiles",
                                                                                             "ComplexTiles" };
        static constexpr std::string_view kTilesDefine      = "DESERT_DEFERRED_TILES";
        static constexpr std::string_view kTileSimpleDefine = "DESERT_DEFERRED_TILE_SIMPLE";
        static constexpr std::string_view kTileModelDefine  = "DESERT_DEFERRED_TILE_MODEL";
        // The model of the SIMPLE list (DeferredTileClassify.shader's SHADING_MODEL_INDEX_DEFAULT_LIT).
        static constexpr std::string_view kTileSimpleModel = "DefaultLit";
        static_assert( DeferredTileLists::kMaxModelLists == Core::ShadingModels::kMaxShadingModels );
        [[nodiscard]] static uint32_t     TileGrid( const uint32_t pixels )
        {
            return ( pixels + kTileSize - 1u ) / kTileSize;
        }
        [[nodiscard]] static std::size_t TiledIndex( const DeferredTileClass tileClass )
        {
            return tileClass == DeferredTileClass::Simple ? kSimpleTiles : kComplexTiles;
        }

        template <typename Block>
        static void DeclareTileReads( RDG::PassBuilder& pass, Block& block, const RDG::BufferRef tiles )
        {
            // A storage read and the indirect-argument read of one buffer in one pass are one use (RDGCompile.cpp:
            // a buffer has no layout), as SSR's trace list.
            block.Storage( DeferredTileShaderParameters::DeferredTilesUB, tiles, RDG::Access::StorageRead );
            pass.Read( tiles, RDG::Access::IndirectArgs );
        }
        // The DeferredLighting shader's layout, kept between frames (follows the shader object and its reload).
        // §1.4: a resource that did not arrive is logged with its reason, never quietly replaced — and a
        // resource that is absent BY DEFINITION is not a failure. The ambient IS the sky light, so the report
        // is keyed on where the sky light is (SkyLightState), not on the bare presence of two cubes:
        //  - NoSky: the scene has no sky, so there is no sky light (UE: no USkyLightComponent) and the black
        //    cubes are the defined answer — said once, at info;
        //  - Ready with a cube that did not resolve, or a missing BRDF LUT (a renderer-global built at
        //    startup and never legitimately absent): a real fault, an ERROR.
        //
        // Edge-triggered, and per RENDERER instance rather than per process: a fullscreen pass runs every
        // frame in every open viewport, so an unconditional log is 60 lines a second times the number of
        // views, and a `static` one would let the second viewport's failure hide behind the first's.
        enum class EnvironmentReport : uint8_t
        {
            Complete,
            NoSky,
            Broken,
        };

        [[nodiscard]] static EnvironmentReport ClassifyEnvironment( const DeferredEnvironmentInput& environment )
        {
            if ( !environment.BrdfLut )
                return EnvironmentReport::Broken;
            switch ( environment.Sky )
            {
                case SkyLightState::NoSky:
                    return EnvironmentReport::NoSky;
                case SkyLightState::Ready:
                    return environment.IsComplete() ? EnvironmentReport::Complete : EnvironmentReport::Broken;
            }
            return EnvironmentReport::Broken;
        }

        void ReportEnvironmentGap( const DeferredEnvironmentInput& environment )
        {
            const EnvironmentReport report = ClassifyEnvironment( environment );
            if ( report == m_EnvironmentReport )
                return;
            m_EnvironmentReport = report;

            switch ( report )
            {
                case EnvironmentReport::Complete:
                    LOG_INFO( "DeferredLighting: the sky light arrived (irradiance, prefiltered, BRDF LUT "
                              "present): the ambient is the sky's from this frame on." );
                    break;
                case EnvironmentReport::NoSky:
                    LOG_INFO( "DeferredLighting: this scene has no sky (no procedural sky, no HDR skybox), so it "
                              "has no sky light: the sky ambient is zero by definition and static geometry is lit "
                              "by its lights, GI and reflection captures only." );
                    break;
                case EnvironmentReport::Broken:
                    LOG_ERROR( "DeferredLighting: the sky light cannot shade the ambient — irradiance cube {}, "
                               "prefiltered cube {}, BRDF LUT {} (sky light {}). Every missing one is bound to "
                               "the engine's EMPTY environment (a black cube), so static opaque geometry gets no "
                               "sky ambient — a deterministic wrong answer rather than the previous scene's sky, "
                               "which is what it used to get (Г14).",
                               environment.Irradiance ? "present" : "MISSING",
                               environment.Prefiltered ? "present" : "MISSING",
                               environment.BrdfLut ? "present" : "MISSING",
                               environment.Sky == SkyLightState::Ready ? "ready" : "not ready" );
                    break;
            }
        }

        // One program per DeferredShadowInput::Sun arm (the index): the cascades, and the
        // DESERT_SUN_SHADOW_SCREEN_MASK permutation reading the screen shadow mask; then the MegaLights program
        // (the screen mask + DESERT_MEGALIGHTS) for Shadows Stochastic, and its cascades twin (DESERT_MEGALIGHTS
        // alone) for Stochastic whose visibility source is the screen (no sun mask is drawn then).
        struct CompositeProgram
        {
            std::shared_ptr<Shader>                   Shader;
            std::shared_ptr<GraphicsPipeline>         Pipeline;
            std::unique_ptr<MaterialDeferredLighting> Material;
            mutable ShaderBindingLayoutCache          Layout;
        };
        static constexpr std::size_t kCascadeProgram       = 0; // DeferredCascadeShadow
        static constexpr std::size_t kScreenMaskSunProgram = 1; // DeferredScreenShadowMask
        static constexpr std::size_t kMegaLightsProgram    = 2; // DeferredShadowInput::MegaLightsLocalLights
        static constexpr std::size_t kRayTracedLocalProgram = 3; // DeferredShadowInput::RayTracedLocalLights
        static constexpr std::size_t kVirtualLocalProgram   = 4; // DeferredShadowInput::VirtualLocalLights
        static constexpr std::size_t kMegaLightsCascadeProgram = 5; // MegaLightsLocalLights under the sun's cascades
        static constexpr std::array<std::string_view, 6> kProgramDebugNames = {
             "DeferredLighting Direct", "DeferredLighting Direct RayTracedSun",
             "DeferredLighting Direct MegaLights", "DeferredLighting Direct RayTracedLocal",
             "DeferredLighting Direct VirtualLocal", "DeferredLighting Direct MegaLightsCascades" };
        // The define of every direct program (DeferredLighting.shader): without it the text is the composite.
        static constexpr std::string_view kDirectDefine = "DESERT_DEFERRED_DIRECT";
        // The Shadows methods each program alone serves; empty = every method needs it (the cascades' program).
        // HardwareTraced and VirtualMaps read the screen mask; Stochastic draws its own MegaLights programs (the
        // sun's mask, or its cascades under StochasticVisibility::Screen).
        static constexpr std::array<Common::Scalability::ShadowMethod, 2> kScreenMaskSunMethods = {
             Common::Scalability::ShadowMethod::VirtualMaps, Common::Scalability::ShadowMethod::HardwareTraced };
        static constexpr std::array<Common::Scalability::ShadowMethod, 1> kMegaLightsMethods = {
             Common::Scalability::ShadowMethod::Stochastic };
        static constexpr std::array<Common::Scalability::ShadowMethod, 1> kRayTracedLocalMethods = {
             Common::Scalability::ShadowMethod::HardwareTraced };
        static constexpr std::array<Common::Scalability::ShadowMethod, 1> kVirtualLocalMethods = {
             Common::Scalability::ShadowMethod::VirtualMaps };
        static constexpr std::array<std::span<const Common::Scalability::ShadowMethod>, 6> kProgramShadowMethods =
             { std::span<const Common::Scalability::ShadowMethod>{}, kScreenMaskSunMethods, kMegaLightsMethods,
               kRayTracedLocalMethods, kVirtualLocalMethods, kMegaLightsMethods };
        // Program @p index made ready to draw: the permutation acquired from the shader service (the cascades'
        // program keeps the shader Initialize looked up), its pipeline over the scene target and its material.
        // On an error the program is left empty and the error is the reason it was refused.
        // Both programs read the G-buffer through the repeat and point states. @p clampSampler: the composite's
        // third state (the sky's cubes, the BRDF table, SSR); the direct program samples nothing through it and
        // does not declare it (DeferredLighting.shader), so binding it there would be a name it lacks.
        template <typename Block>
        static void DeclareGBufferReads( Block& block, const DeferredCompositeInputs& inputs,
                                         const bool clampSampler )
        {
            constexpr RDG::Access           kRead  = RDG::Access::SampledGraphics;
            constexpr RDG::SubresourceRange kWhole = RDG::SubresourceRange::All();
            if ( clampSampler )
                block.Sampler( DeferredLightingCompositeShaderParameters::LinearClampSampler,
                               RDG::SamplerDesc::LinearClamp() );
            block.Sampler( DeferredLightingCompositeShaderParameters::LinearRepeatSampler,
                           RDG::SamplerDesc::LinearRepeat() )
                 .Sampler( DeferredLightingCompositeShaderParameters::PointClampSampler,
                           RDG::SamplerDesc::PointClamp() )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferATexture, inputs.GBufferA, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferBTexture, inputs.GBufferB, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferShadingWordTexture,
                           inputs.GBufferShadingWord, kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferDepthTexture, inputs.GBufferDepth,
                           kRead, kWhole );
        }
        // A direct program: its permutation of the shader, drawn into the direct target (one RGBA32F colour).
        Common::BoolResultStr BuildProgram( const std::size_t index )
        {
            CompositeProgram& program = m_Programs[index];
            program                   = CompositeProgram{};
            auto acquired = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant(
                 "DeferredLighting", ProgramVariant( index ) );
            if ( !acquired )
                return Common::MakeError( std::string( acquired.GetError() ) );
            program.Shader = acquired.ExtractValue();
            const RenderTargetLayout direct{ .ColorFormats = { ViewTargetFormats::kDeferredDirectLighting },
                                             .DepthFormat  = std::nullopt,
                                             .Samples      = 1 };
            return BuildPipeline( program, direct, kProgramDebugNames[index] );
        }
        // The composite: the shader without defines (Initialize's), drawn into the scene target.
        Common::BoolResultStr BuildComposite()
        {
            m_Composite = CompositeProgram{ .Shader = std::move( m_Composite.Shader ) };
            return BuildPipeline( m_Composite, SceneTargetLayout(), "DeferredLighting Composite" );
        }
        Common::BoolResultStr BuildPipeline( CompositeProgram& program, const RenderTargetLayout& targetLayout,
                                             const std::string_view debugName )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
            {
                program = CompositeProgram{};
                return Common::MakeError( "Deferred lighting target framebuffer missing" );
            }
            GraphicsPipelineSpecification spec;
            spec.TargetLayout      = targetLayout;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            spec.DebugName         = std::string( debugName );
            spec.Shader            = program.Shader;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
            {
                program = CompositeProgram{};
                return Common::MakeError( std::string( pipeline.GetError() ) );
            }
            program.Pipeline = pipeline.GetValue();
            program.Material = std::make_unique<MaterialDeferredLighting>( program.Shader );
            return BOOLSUCCESS;
        }
        [[nodiscard]] static Common::BoolResultStr DrawClass( const RDG::PassBindings& bindings,
                                                              const CompositeProgram&  program,
                                                              const DeferredTileClass  tileClass,
                                                              const RDG::BufferRef     tiles )
        {
            if ( tileClass == DeferredTileClass::Fullscreen )
                return Renderer::GetInstance().DrawFullscreen( bindings, *program.Pipeline,
                                                               program.Material->GetMaterialExecutor() );
            const uint64_t offset =
                 tileClass == DeferredTileClass::Simple ? kSimpleArgsOffset : kComplexArgsOffset;
            return Renderer::DrawProceduralIndirect( bindings, *program.Pipeline,
                                                     program.Material->GetMaterialExecutor(), tiles, offset );
        }
        [[nodiscard]] const CompositeProgram& DirectProgramOf( const std::size_t      program,
                                                               const DeferredTileClass tileClass ) const
        {
            if ( tileClass == DeferredTileClass::Fullscreen )
                return m_Programs[program];
            return m_TiledPrograms[program][TiledIndex( tileClass )];
        }
        [[nodiscard]] const CompositeProgram& CompositeProgramOf( const DeferredTileClass tileClass ) const
        {
            if ( tileClass == DeferredTileClass::Fullscreen )
                return m_Composite;
            return m_TiledComposite[TiledIndex( tileClass )];
        }
        // The tiled permutations of every direct program that built and of the composite, each one missing
        // acquired and built again. One that does not build is said here with its reason; the view then draws
        // the whole screen (TileClassifyReady is false for its arm), the picture is the same.
        void BuildTiledPrograms()
        {
            const RenderTargetLayout direct{ .ColorFormats = { ViewTargetFormats::kDeferredDirectLighting },
                                             .DepthFormat  = std::nullopt,
                                             .Samples      = 1 };
            for ( std::size_t tiled = 0; tiled < kTiledClassCount; ++tiled )
            {
                for ( std::size_t index = 0; index < m_Programs.size(); ++index )
                {
                    CompositeProgram& program = m_TiledPrograms[index][tiled];
                    if ( !m_Programs[index].Pipeline || program.Pipeline )
                        continue;
                    const std::string name =
                         std::format( "{} {}", kProgramDebugNames[index], kTiledDebugNames[tiled] );
                    const Core::ShaderVariant variant = TileVariant( ProgramVariant( index ), tiled );
                    if ( const Common::BoolResultStr built = BuildVariant( program, variant, direct, name ); !built )
                        LOG_ERROR( "Deferred lighting: {}: {} (the view draws this arm fullscreen)", name,
                                   built.GetError() );
                }
                CompositeProgram& composite = m_TiledComposite[tiled];
                if ( composite.Pipeline )
                    continue;
                const std::string name = std::format( "DeferredLighting Composite {}", kTiledDebugNames[tiled] );
                const Core::ShaderVariant  variant = TileVariant( Core::ShaderVariant{}, tiled );
                const RenderTargetLayout   scene   = SceneTargetLayout();
                if ( const Common::BoolResultStr built = BuildVariant( composite, variant, scene, name ); !built )
                    LOG_ERROR( "Deferred lighting: {}: {} (the view draws fullscreen)", name, built.GetError() );
            }
            // FPS-DIRECT-TILES: a model has its own list when its folded permutation built for every arm that
            // built; else its single-model tiles stay mixed (the dispatch: the same picture), said here.
            uint32_t mask = 0u;
            for ( const uint32_t model : TileModels() )
            {
                bool every = true;
                for ( std::size_t index = 0; index < m_Programs.size(); ++index )
                {
                    CompositeProgram& program = m_ModelPrograms[index][model];
                    if ( !m_Programs[index].Pipeline )
                        continue;
                    if ( program.Pipeline )
                        continue;
                    const std::string name = std::format( "{} Model{}Tiles", kProgramDebugNames[index], model );
                    const Core::ShaderVariant variant = ModelVariant( ProgramVariant( index ), model );
                    if ( const Common::BoolResultStr built = BuildVariant( program, variant, direct, name ); !built )
                    {
                        LOG_ERROR( "Deferred lighting: {}: {} (model {}'s tiles draw mixed)", name,
                                   built.GetError(), model );
                        every = false;
                    }
                }
                if ( every )
                    mask |= 1u << model;
            }
            m_ModelListMask = mask;
        }
        Common::BoolResultStr BuildVariant( CompositeProgram& program, const Core::ShaderVariant& variant,
                                            const RenderTargetLayout& targetLayout, const std::string_view name )
        {
            program       = CompositeProgram{};
            auto acquired = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "DeferredLighting",
                                                                                           variant );
            if ( !acquired )
                return Common::MakeError( std::string( acquired.GetError() ) );
            program.Shader = acquired.ExtractValue();
            return BuildPipeline( program, targetLayout, name );
        }
        void RefuseProgram( const std::size_t index, const std::string_view reason )
        {
            for ( const Common::Scalability::ShadowMethod method : kProgramShadowMethods[index] )
                AddRefusal( m_Refusals.Shadows, method, kProgramDebugNames[index], reason );
        }
        std::array<CompositeProgram, 6> m_Programs; // "Deferred: Direct Lighting", one per shadow arm
        CompositeProgram                m_Composite; // "Deferred: Composite": the shader without defines
        ProgramRefusals                 m_Refusals;
        std::size_t                     m_Active = kCascadeProgram; // FillMaterial's arm, this frame
        // Their tiled permutations (TileVariant): [arm][kSimpleTiles / kComplexTiles], and the composite's [list].
        std::array<std::array<CompositeProgram, kTiledClassCount>, 6> m_TiledPrograms;
        std::array<CompositeProgram, kTiledClassCount>                m_TiledComposite;
        // The programs the declares chose this frame, by DeferredTileClass (null: none could draw); the execs
        // draw them.
        mutable std::array<const CompositeProgram*, 3> m_DeclaredPrograms{}; // the direct passes'
        mutable std::array<const CompositeProgram*, 3> m_DeclaredComposites{};
        // FPS-DIRECT-TILES: the per-model permutations (ModelVariant), [arm][model index]; the models whose list
        // the classify fills (bit m: model m's permutation built for every arm that built); and the ones the
        // complex tiles' direct pass declared this frame, in block order (block i + 1).
        std::array<std::array<CompositeProgram, DeferredTileLists::kMaxModelLists>, 6> m_ModelPrograms;
        uint32_t                                                                      m_ModelListMask = 0u;
        struct DeclaredModel
        {
            const CompositeProgram* Program = nullptr;
            uint32_t                Model   = 0u;
        };
        mutable std::array<DeclaredModel, DeferredTileLists::kMaxModelLists> m_DeclaredModels{};
        mutable uint32_t                                                     m_DeclaredModelCount = 0u;

        // "Deferred: Tile Classify": the tile lists the tiled permutations draw.
        mutable ShaderBindingLayoutCache m_ClassifyLayout;
        std::shared_ptr<Shader>          m_ClassifyShader;
        std::shared_ptr<ComputePipeline> m_ClassifyPipeline;

        // "LightGrid": the froxel light lists the composite reads.
        mutable ShaderBindingLayoutCache m_GridLayout;
        std::shared_ptr<Shader>          m_GridShader;
        std::shared_ptr<ComputePipeline> m_GridPipeline;

        // "Debug: LightGridWorld" (DeferredDebugMode::LightGridWorld): one pipeline per post-input format.
        mutable ShaderBindingLayoutCache                                      m_WorldViewLayout;
        std::shared_ptr<Shader>                                               m_WorldViewShader;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_WorldViewPipelines;
        std::shared_ptr<GraphicsPipeline>                                     m_WorldView;

        // Starts true so the first INCOMPLETE frame is the edge that logs; a renderer that never loses
        // its environment says nothing at all.
        // What was last said about the environment; Complete at birth, so a view whose sky light is there on
        // its first frame says nothing.
        EnvironmentReport m_EnvironmentReport = EnvironmentReport::Complete;
    };
} // namespace Desert::Graphic::System

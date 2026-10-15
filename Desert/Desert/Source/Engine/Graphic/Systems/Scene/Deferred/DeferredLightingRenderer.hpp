#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ShaderProtocols/LightPayloadBytes.hpp>
#include <Engine/Graphic/LocalShadows.hpp>
#include <Engine/Graphic/LightGrid.hpp>

#include <glm/glm.hpp>

#include <array>
#include <map>
#include <utility>
#include <span>
#include <type_traits>
#include <string_view>

namespace Desert::Graphic::System
{
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
        RDG::TextureRef SSR; // SSR's resolved reflection (rgb, a = coverage), or System.Black (the environment's)
        // The traced GI's final gather, per pixel: diffuse irradiance and specular radiance (FrameTransients::
        // GIDiffuse / GISpecular, SceneRenderer::AddFrameTracedGI), or System.Black (unread in the other GI
        // modes).
        RDG::TextureRef GIDiffuse;
        RDG::TextureRef GISpecular;
        // The scene/view inputs (SceneViewInputsOf): cascades, environment cubes, BRDF LUT, cloud shadow map.
        SceneViewInputs View;
    };

    // The view's lights as graph buffers (ViewLightBuffers, uploaded by the graph's upload command before the
    // nodes that read them) plus the CPU copy of the grid's parameters: the dispatch size of "LightGrid".
    struct DeferredCompositeLights : ViewLightBuffers
    {
        LightGridParamsPayload Grid;
    };

    // The block name of the local shadow view table in the composite shader (Mesh/LocalLightShadow.glslh).
    inline constexpr std::string_view kLocalShadowViewsBlockName = "LocalShadowViewsUB";

    // Deferred lighting + G-buffer debug pass. Fullscreen: reads the scene renderer's MRT G-buffer and writes
    // the shaded (or debug) result into the scene target framebuffer, which the post chain then tonemaps.
    // Runs in the manual chain (like Tonemap), only when RenderPath == Deferred.
    class DeferredLightingRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DeferredLighting" );
            if ( !m_Shader )
                return Common::MakeError( "DeferredLighting shader not found" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "Deferred lighting target framebuffer missing" );

            GraphicsPipelineSpecification spec;
            spec.DebugName         = "DeferredLighting";
            spec.TargetLayout      = SceneTargetLayout();
            spec.Shader            = m_Shader;
            // Fullscreen composite over the forward-rendered scene: no depth test/write (the quad has no
            // meaningful depth), and LOAD the target so the real sky/grid drawn by the forward passes are
            // preserved — the shader discards non-geometry texels so that scene shows through.
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();

            m_Material = std::make_unique<MaterialDeferredLighting>();

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
            return BOOLSUCCESS;
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
            pass.Bindings( m_WorldViewLayout.Get( m_WorldViewShader ), RDG::OtherRouteFill{} )
                 .Sampled( "u_Depth", depth, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearRepeat() )
                 .Storage( "LightGridWorldViewUB", view, RDG::Access::StorageRead )
                 .Storage( kLightGridParamsBlockName, lights.WorldGridParams, RDG::Access::StorageRead )
                 .Storage( kLightGridWorldCellsBlockName, lights.WorldGridCells, RDG::Access::StorageRead );
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
                 .Storage( kLightGridParamsBlockName, world ? lights.WorldGridParams : lights.GridParams,
                           RDG::Access::StorageRead )
                 .Storage( ShaderProtocols::PointLight::Name, lights.Point, RDG::Access::StorageRead )
                 .Storage( ShaderProtocols::SpotLight::Name, lights.Spot, RDG::Access::StorageRead )
                 .Storage( kLightGridCellsBlockName, world ? lights.WorldGridCells : lights.GridCells,
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
        // = camera world position; debugMode selects a raw channel (0 = lit); giMode picks the indirect-light
        // source (0 = off, 1 = screen-space, 2 = RSM).
        void FillMaterial( const glm::vec4& lightDir, const glm::vec4& lightColor, const glm::vec4& cameraPos,
                           const glm::mat4& invJitteredViewProjection, int debugMode, uint32_t pointCount,
                           uint32_t spotCount, const DeferredShadowInput& shadow, float giIntensity,
                           bool ssaoEnabled, int giMode, const CloudShadowInput& cloudShadow,
                           const DeferredEnvironmentInput& environment, const glm::mat4& jitteredViewProjection )
        {
            if ( !m_Material )
                return;
            ReportEnvironmentGap( environment );
            m_Material->BindInputs( lightDir, lightColor, cameraPos, invJitteredViewProjection, debugMode,
                                    pointCount, spotCount, shadow, giIntensity, ssaoEnabled, giMode, cloudShadow,
                                    environment, jitteredViewProjection );
        }

        // SETUP of "Deferred: Composite", after FillMaterial: the node's one block (block 0). The G-buffer and AO
        // with the sampler the material route sampled them with (the image's own: linear, REPEAT, all mips),
        // the two light buffers as StorageRead entries, the scene/view inputs the shader samples.
        void DeclareCompositeBindings( RDG::PassBuilder& pass, const DeferredCompositeInputs& inputs,
                                       const ViewLightBuffers& lights ) const
        {
            if ( !m_Pipeline || !m_Material )
                return;
            constexpr RDG::SamplerDesc kSampler = RDG::SamplerDesc::LinearRepeat();
            const auto&                layout   = m_BindingLayout.Get( m_Shader );
            auto block = pass.Bindings( layout, m_Material->GetMaterialExecutor()->GetRouteFill() );
            block.Sampled( "u_GBufferA", inputs.GBufferA, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), kSampler )
                 .Sampled( "u_GBufferB", inputs.GBufferB, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), kSampler )
                 // The word is an integer (texelFetch only) and depth is never filtered: both point-sampled.
                 .Sampled( "u_GBufferShadingWord", inputs.GBufferShadingWord, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GBufferDepth", inputs.GBufferDepth, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GBufferEmissive", inputs.GBufferEmissive, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), kSampler )
                 .Sampled( "u_SSAO", inputs.SSAO, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           kSampler )
                 .Sampled( "u_SSR", inputs.SSR, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearClamp() )
                 .Storage( ShaderProtocols::PointLight::Name, lights.Point, RDG::Access::StorageRead )
                 .Storage( ShaderProtocols::SpotLight::Name, lights.Spot, RDG::Access::StorageRead )
                 .Storage( kLocalShadowViewsBlockName, lights.ShadowViews, RDG::Access::StorageRead )
                 // One texel per pixel, at the G-buffer's extent: point-sampled.
                 .Sampled( "u_GIDiffuse", inputs.GIDiffuse, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GISpecular", inputs.GISpecular, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() );
            if ( layout )
                BindSceneViewInputs( block, inputs.View, *layout );
        }

        // EXEC: shades the G-buffer into the scene target, inside the render pass the frame graph opens on it
        // with LOAD (which preserves the forward-rendered sky/grid; the shader discards non-geometry texels),
        // from block 0 that DeclareCompositeBindings declared.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context )
        {
            if ( !m_Pipeline || !m_Material )
                return Common::MakeError(
                     "Deferred: Composite: the deferred-lighting pipeline is not initialised" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
        }

    private:
        // The DeferredLighting shader's layout, kept between frames (follows the shader object and its reload).
        mutable ShaderBindingLayoutCache m_BindingLayout;
        // §1.4: a resource that did not arrive is logged with its reason, never quietly replaced — and a
        // resource that is absent BY DEFINITION is not a failure. The ambient IS the sky light, so the report
        // is keyed on where the sky light is (SkyLightState), not on the bare presence of two cubes:
        //  - NoSky: the scene has no sky, so there is no sky light (UE: no USkyLightComponent) and the black
        //    cubes are the defined answer — said once, at info;
        //  - Settling: the sky's environment is on its way (the first frame of a physical sky waits for this
        //    frame's atmosphere LUTs, then the bake lands asynchronously) — said once, at info, and the
        //    "arrived" line below closes it, so a bake that never lands is visible as an unclosed wait;
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
            Settling,
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
                case SkyLightState::Settling:
                    return EnvironmentReport::Settling;
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
                case EnvironmentReport::Settling:
                    LOG_INFO( "DeferredLighting: the sky light is still being captured (the sky's environment "
                              "bake has not landed); the sky ambient is black until it does." );
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

        std::shared_ptr<Shader>                   m_Shader;
        std::shared_ptr<GraphicsPipeline>         m_Pipeline;
        std::unique_ptr<MaterialDeferredLighting> m_Material;

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

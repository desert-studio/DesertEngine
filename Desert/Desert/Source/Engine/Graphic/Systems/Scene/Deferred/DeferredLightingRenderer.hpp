#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
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
#include <string>
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
        RDG::TextureRef GI;              // RSM-GI accumulation, or System.Black (no indirect)
        RDG::TextureRef SunShadowMask; // "Shadows: Ray Traced Sun" (DeferredScreenShadowMask); bound by its program only
        RDG::TextureRef SSR; // SSR's resolved reflection (rgb, a = coverage), or System.Black (the environment's)
        // The screen probe gather's per-pixel diffuse irradiance and specular radiance (a traced GI method,
        // SceneRenderer::AddFrameTracedGI), or System.Black (unread in the other GI modes).
        RDG::TextureRef ScreenProbeDiffuse;
        RDG::TextureRef ScreenProbeSpecular;
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
auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
            m_Programs[kCascadeProgram].Shader = shaders->GetByName( "DeferredLighting" );
            if ( !m_Programs[kCascadeProgram].Shader )
                return Common::MakeError( "DeferredLighting shader not found" );
            // The permutation of "Shadows: Ray Traced Sun": u_ScreenShadowMask in place of u_ShadowCascades.
            m_Programs[kRayTracedSunProgram].Shader = shaders->AcquireVariant(
                 "DeferredLighting", Core::ShaderVariant{ .Defines = { std::string( kSunShadowRayQueryDefine ) } } );
            if ( !m_Programs[kRayTracedSunProgram].Shader )
                return Common::MakeError( "DeferredLighting (DESERT_SUN_SHADOW_RAY_QUERY) did not compile" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "Deferred lighting target framebuffer missing" );

            GraphicsPipelineSpecification spec;
            spec.TargetLayout      = SceneTargetLayout();
            // Fullscreen composite over the forward-rendered scene: no depth test/write (the quad has no
            // meaningful depth), and LOAD the target so the real sky/grid drawn by the forward passes are
            // preserved — the shader discards non-geometry texels so that scene shows through.
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            for ( std::size_t index = 0; index < m_Programs.size(); ++index )
            {
                CompositeProgram& program = m_Programs[index];
                // Each permutation names its own pipeline, so a device-lost report says which sun arm drew.
                const std::string programName{ kProgramDebugNames[index] };
                spec.DebugName = programName;
                spec.Shader    = program.Shader;
                const auto pipeline = Graphic::GraphicsPipeline::Create( spec );
                if ( !pipeline )
                    return Common::MakeError( pipeline.GetError() );
                program.Pipeline = pipeline.GetValue();
                program.Material = std::make_unique<MaterialDeferredLighting>( program.Shader );
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
                 .Texture( "u_Depth", depth, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
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
            // The sun's arm picks the program this frame's composite draws with.
            m_Active                         = shadow.Sun.index();
            const CompositeProgram& program = m_Programs[m_Active];
            if ( !program.Material )
                return;
            ReportEnvironmentGap( environment );
            program.Material->BindInputs( lightDir, lightColor, cameraPos, invJitteredViewProjection, debugMode,
                                    pointCount, spotCount, shadow, giIntensity, ssaoEnabled, giMode, cloudShadow,
                                    environment, jitteredViewProjection );
        }

        // SETUP of "Deferred: Composite", after FillMaterial: the node's one block (block 0). UE's shared-sampler
        // form: every input is a separate texture and the shader reads each through one of three sampler states
        // the pass binds once (the G-buffer, AO and GI linear REPEAT, as the material route sampled them; the
        // integer shading word and depth point-clamped; the LUT, the cubes and the reflection captures
        // linear-clamped). One combined sampler per input was 17 in the fragment stage, over MoltenVK's 16
        // (maxPerStageDescriptorSamplers), and the device refused the program. Then the two light buffers as
        // StorageRead entries and the scene/view inputs the shader samples.
        void DeclareCompositeBindings( RDG::PassBuilder& pass, const DeferredCompositeInputs& inputs,
                                       const ViewLightBuffers& lights ) const
        {
            const CompositeProgram& program = m_Programs[m_Active];
            if ( !program.Pipeline || !program.Material )
                return;
            constexpr RDG::Access           kRead  = RDG::Access::SampledGraphics;
            constexpr RDG::SubresourceRange kWhole = RDG::SubresourceRange::All();
            const auto&                     layout = program.Layout.Get( program.Shader );
            auto block = pass.Bindings( layout, program.Material->GetMaterialExecutor()->GetRouteFill() );
            block.Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
                 .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                 .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                 .Texture( "u_GBufferA", inputs.GBufferA, kRead, kWhole )
                 .Texture( "u_GBufferB", inputs.GBufferB, kRead, kWhole )
                 .Texture( "u_GBufferShadingWord", inputs.GBufferShadingWord, kRead, kWhole )
                 .Texture( "u_GBufferDepth", inputs.GBufferDepth, kRead, kWhole )
                 .Texture( "u_GBufferEmissive", inputs.GBufferEmissive, kRead, kWhole )
                 .Texture( "u_SSAO", inputs.SSAO, kRead, kWhole )
                 .Texture( "u_GI", inputs.GI, kRead, kWhole )
                 // The resolved reflections, read through the clamped state (UE: the SSR composite input).
                 .Texture( "u_SSR", inputs.SSR, kRead, kWhole )
                 .Storage( ShaderProtocols::PointLight::Name, lights.Point, RDG::Access::StorageRead )
                 .Storage( ShaderProtocols::SpotLight::Name, lights.Spot, RDG::Access::StorageRead )
                 .Storage( kLocalShadowViewsBlockName, lights.ShadowViews, RDG::Access::StorageRead )
                 // One texel per pixel, read through the point-clamped state like the depth.
                 .Texture( "u_ScreenProbeDiffuse", inputs.ScreenProbeDiffuse, kRead, kWhole )
                 .Texture( "u_ScreenProbeSpecular", inputs.ScreenProbeSpecular, kRead, kWhole );
            if ( m_Active == kRayTracedSunProgram )
                block.Texture( "u_ScreenShadowMask", inputs.SunShadowMask, kRead, kWhole );
            if ( layout )
                BindSceneViewInputs( block, inputs.View, *layout );
        }

        // EXEC: shades the G-buffer into the scene target, inside the render pass the frame graph opens on it
        // with LOAD (which preserves the forward-rendered sky/grid; the shader discards non-geometry texels),
        // from block 0 that DeclareCompositeBindings declared.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context )
        {
            const CompositeProgram& program = m_Programs[m_Active];
            if ( !program.Pipeline || !program.Material )
                return Common::MakeError(
                     "Deferred: Composite: the deferred-lighting pipeline is not initialised" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *program.Pipeline,
                                                           program.Material->GetMaterialExecutor() );
        }

    private:
        // The DeferredLighting shader's layout, kept between frames (follows the shader object and its reload).
        // §1.4: a resource that did not arrive is logged with its reason, never quietly replaced. There is
        // no "no environment" mode here — the ambient IS the environment, so an incomplete set means the
        // IBL bake failed or never ran, and every static opaque surface in the frame is about to be shaded
        // by whatever the descriptor fallback happens to be. Say which of the three is missing; a bake that
        // produced two cubes and no LUT is a different failure from one that produced nothing.
        //
        // Edge-triggered, and per RENDERER instance rather than per process: a fullscreen pass runs every
        // frame in every open viewport, so an unconditional log is 60 lines a second times the number of
        // views, and a `static` one would let the second viewport's failure hide behind the first's.
        void ReportEnvironmentGap( const DeferredEnvironmentInput& environment )
        {
            const bool complete = environment.IsComplete();
            if ( complete == m_EnvironmentWasComplete )
                return;
            m_EnvironmentWasComplete = complete;

            // The other edge too: a physical sky's first bake waits one frame for the atmosphere LUTs this
            // frame's graph writes (SkyboxRenderer), so every new view opens with the gap below; the line that
            // says the ambient arrived is what tells that one-frame wait from a bake that never landed.
            if ( complete )
                LOG_INFO( "DeferredLighting: the baked environment arrived (irradiance, prefiltered, BRDF LUT "
                          "present): the ambient is the sky's from this frame on." );
            if ( !complete )
                LOG_ERROR( "DeferredLighting: no baked environment to shade the ambient with — irradiance "
                           "cube {}, prefiltered cube {}, BRDF LUT {}. Every missing one is bound to the "
                           "engine's EMPTY environment (a black cube), so static opaque geometry gets no "
                           "ambient at all — a deterministic wrong answer rather than the previous scene's "
                           "sky, which is what it used to get (Г14).",
                           environment.Irradiance ? "present" : "MISSING",
                           environment.Prefiltered ? "present" : "MISSING",
                           environment.BrdfLut ? "present" : "MISSING" );
        }

        // One program per DeferredShadowInput::Sun arm (the index): the cascades, and the
        // DESERT_SUN_SHADOW_RAY_QUERY permutation reading the screen shadow mask.
        struct CompositeProgram
        {
            std::shared_ptr<Shader>                   Shader;
            std::shared_ptr<GraphicsPipeline>         Pipeline;
            std::unique_ptr<MaterialDeferredLighting> Material;
            mutable ShaderBindingLayoutCache          Layout;
        };
        static constexpr std::size_t kCascadeProgram      = 0; // DeferredCascadeShadow
        static constexpr std::size_t kRayTracedSunProgram = 1; // DeferredScreenShadowMask
        static constexpr std::array<std::string_view, 2> kProgramDebugNames = { "DeferredLighting",
                                                                                "DeferredLighting RayTracedSun" };
        std::array<CompositeProgram, 2> m_Programs;
        std::size_t                     m_Active = kCascadeProgram; // FillMaterial's arm, this frame

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
        bool m_EnvironmentWasComplete = true;
    };
} // namespace Desert::Graphic::System

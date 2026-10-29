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
#include <Engine/Graphic/RenderMethods.hpp>

#include <glm/glm.hpp>

#include <Common/Core/Logger.hpp>

#include <array>
#include <format>
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

        // The permutation of the composite program @p program (kScreenMaskSunProgram..kRayTracedLocalProgram; the
        // cascades' program is the default one), cooked: ShippedByCook - the one statement Initialize acquires and
        // ProgramVariants lists.
        [[nodiscard]] static Core::ShaderVariant ProgramVariant( const std::size_t program )
        {
            Core::ShaderVariant variant{ .Defines = { std::string( kSunShadowScreenMaskDefine ) } };
            if ( program == kMegaLightsProgram )
                variant.Defines.emplace_back( kMegaLightsDefine );
            if ( program == kRayTracedLocalProgram )
                variant.Defines.emplace_back( kLocalShadowScreenMaskDefine );
            variant.ShippedByCook = true;
            return variant;
        }

        // Every permutation the renderer acquires, cooked by the package (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants()
        {
            std::vector<Core::CookedProgramVariant> variants;
            for ( std::size_t index = kScreenMaskSunProgram; index < kProgramDebugNames.size(); ++index )
                variants.push_back( { "DeferredLighting", ProgramVariant( index ) } );
            return variants;
        }

        virtual Common::BoolResultStr Initialize() override
        {
            m_Programs[kCascadeProgram].Shader =
                 Runtime::ResourceRegistry::GetShaderService()->GetByName( "DeferredLighting" );
            if ( !m_Programs[kCascadeProgram].Shader )
                return Common::MakeError( "DeferredLighting shader not found" );
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
            return BOOLSUCCESS;
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
                           const DeferredEnvironmentInput& environment, const glm::mat4& jitteredViewProjection )
        {
            // The sun's arm picks the program this frame's composite draws with; MegaLights' local lights pick
            // theirs (the sun is then the ray-traced mask: SceneRenderer resolves Stochastic's sun to it).
            m_Active = shadow.MegaLightsLocalLights  ? kMegaLightsProgram
                       : shadow.RayTracedLocalLights ? kRayTracedLocalProgram
                                                     : shadow.Sun.index();
            const CompositeProgram& program = m_Programs[m_Active];
            if ( !program.Material )
                return;
            ReportEnvironmentGap( environment );
            program.Material->BindInputs( lightDir, lightColor, light1Dir, light1Color, cameraPos,
                                          invJitteredViewProjection, debugMode,
                                          pointCount, spotCount, shadow, gi, ssaoEnabled, cloudShadow, environment,
                                          jitteredViewProjection );
        }

        // SETUP of "Deferred: Composite", after FillMaterial: the node's one block (block 0). UE's shared-sampler
        // form: every input is a separate texture and the shader reads each through one of three sampler states
        // the pass binds once (the G-buffer and AO linear REPEAT, as the material route sampled them; the
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
            block.Sampler( DeferredLightingCompositeShaderParameters::LinearRepeatSampler,
                           RDG::SamplerDesc::LinearRepeat() )
                 .Sampler( DeferredLightingCompositeShaderParameters::LinearClampSampler,
                           RDG::SamplerDesc::LinearClamp() )
                 .Sampler( DeferredLightingCompositeShaderParameters::PointClampSampler,
                           RDG::SamplerDesc::PointClamp() )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferATexture, inputs.GBufferA, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferBTexture, inputs.GBufferB, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferShadingWordTexture,
                           inputs.GBufferShadingWord, kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferDepthTexture, inputs.GBufferDepth,
                           kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GBufferEmissiveTexture,
                           inputs.GBufferEmissive, kRead, kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::SsaoTexture, inputs.SSAO, kRead, kWhole )
                 // The resolved reflections, read through the clamped state (UE: the SSR composite input).
                 .Texture( DeferredLightingCompositeShaderParameters::SsrTexture, inputs.SSR, kRead, kWhole )
                 // One texel per pixel, read through the point-clamped state like the depth.
                 .Texture( DeferredLightingCompositeShaderParameters::GiDiffuseTexture, inputs.GIDiffuse, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GiSpecularTexture, inputs.GISpecular, kRead,
                           kWhole )
                 .Texture( DeferredLightingCompositeShaderParameters::GiBackfaceTexture, inputs.GIBackface, kRead,
                           kWhole )
                 .Storage( DeferredLightingCompositeShaderParameters::SubsurfaceDiffuseTexture,
                           inputs.SubsurfaceDiffuse, RDG::Access::StorageWrite );
            // The light rows and their shadow views: the light-grid loop's. The MegaLights program reads
            // MegaLights' lighting instead.
            if ( m_Active == kMegaLightsProgram )
                block.Texture( DeferredLightingCompositeShaderParameters::MegaLightsDiffuseTexture,
                               inputs.MegaLightsDiffuse, kRead, kWhole )
                     .Texture( DeferredLightingCompositeShaderParameters::MegaLightsSpecularTexture,
                               inputs.MegaLightsSpecular, kRead, kWhole );
            else
                block.Storage( ShaderProtocols::PointLight::Parameter, lights.Point, RDG::Access::StorageRead )
                     .Storage( ShaderProtocols::SpotLight::Parameter, lights.Spot, RDG::Access::StorageRead );
            if ( m_Active == kScreenMaskSunProgram || m_Active == kMegaLightsProgram ||
                 m_Active == kRayTracedLocalProgram )
                block.Texture( DeferredLightingCompositeShaderParameters::ScreenShadowMaskTexture,
                               inputs.SunShadowMask, kRead, kWhole );
            if ( m_Active == kRayTracedLocalProgram )
                block.Texture( DeferredLightingCompositeShaderParameters::LocalShadowMasksTexture,
                               inputs.LocalShadowMasks, kRead, kWhole );
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
        // (the screen mask + DESERT_MEGALIGHTS) for Shadows Stochastic.
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
        static constexpr std::array<std::string_view, 4> kProgramDebugNames = {
             "DeferredLighting", "DeferredLighting RayTracedSun", "DeferredLighting MegaLights",
             "DeferredLighting RayTracedLocal" };
        // The Shadows methods each program alone serves; empty = every method needs it (the cascades' program).
        // HardwareTraced and VirtualMaps read the screen mask; Stochastic draws its own MegaLights program.
        static constexpr std::array<Common::Scalability::ShadowMethod, 2> kScreenMaskSunMethods = {
             Common::Scalability::ShadowMethod::VirtualMaps, Common::Scalability::ShadowMethod::HardwareTraced };
        static constexpr std::array<Common::Scalability::ShadowMethod, 1> kMegaLightsMethods = {
             Common::Scalability::ShadowMethod::Stochastic };
        static constexpr std::array<Common::Scalability::ShadowMethod, 1> kRayTracedLocalMethods = {
             Common::Scalability::ShadowMethod::HardwareTraced };
        static constexpr std::array<std::span<const Common::Scalability::ShadowMethod>, 4> kProgramShadowMethods =
             { std::span<const Common::Scalability::ShadowMethod>{}, kScreenMaskSunMethods, kMegaLightsMethods,
               kRayTracedLocalMethods };
        // Program @p index made ready to draw: the permutation acquired from the shader service (the cascades'
        // program keeps the shader Initialize looked up), its pipeline over the scene target and its material.
        // On an error the program is left empty and the error is the reason it was refused.
        Common::BoolResultStr BuildProgram( const std::size_t index )
        {
            CompositeProgram& program = m_Programs[index];
            program                   = CompositeProgram{ .Shader = std::move( program.Shader ) };
            // The screen-mask permutation (the ray-traced sun, the virtual shadow map): ScreenShadowMaskTexture in
            // place of ShadowCascadesTexture.
            // Shadows Stochastic: the ray-traced sun's mask, and the local lights from MegaLights in place of the
            // light-grid loop and its shadow maps. Shadows HardwareTraced: the ray-traced sun's mask, and the
            // local lights' ray-traced masks by slot. Each from the one list the cook ships (ProgramVariants).
            if ( index != kCascadeProgram )
            {
                auto acquired = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant(
                     "DeferredLighting", ProgramVariant( index ) );
                if ( !acquired )
                {
                    program = CompositeProgram{};
                    return Common::MakeError( std::string( acquired.GetError() ) );
                }
                program.Shader = acquired.ExtractValue();
            }
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
            {
                program = CompositeProgram{};
                return Common::MakeError( "Deferred lighting target framebuffer missing" );
            }
            GraphicsPipelineSpecification spec;
            spec.TargetLayout = SceneTargetLayout();
            // Fullscreen composite over the forward-rendered scene: no depth test/write (the quad has no
            // meaningful depth), and LOAD the target so the real sky/grid drawn by the forward passes are
            // preserved — the shader discards non-geometry texels so that scene shows through.
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            // Each permutation names its own pipeline, so a device-lost report says which sun arm drew.
            spec.DebugName      = std::string( kProgramDebugNames[index] );
            spec.Shader         = program.Shader;
            const auto pipeline = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
            {
                program = CompositeProgram{};
                return Common::MakeError( std::string( pipeline.GetError() ) );
            }
            program.Pipeline = pipeline.GetValue();
            program.Material = std::make_unique<MaterialDeferredLighting>( program.Shader );
            return BOOLSUCCESS;
        }
        // Each Shadows method program @p index alone serves is refused by its name for @p reason.
        void RefuseProgram( const std::size_t index, const std::string_view reason )
        {
            for ( const Common::Scalability::ShadowMethod method : kProgramShadowMethods[index] )
                AddRefusal( m_Refusals.Shadows, method, kProgramDebugNames[index], reason );
        }
        std::array<CompositeProgram, 4> m_Programs;
        ProgramRefusals                 m_Refusals;
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
        // What was last said about the environment; Complete at birth, so a view whose sky light is there on
        // its first frame says nothing.
        EnvironmentReport m_EnvironmentReport = EnvironmentReport::Complete;
    };
} // namespace Desert::Graphic::System

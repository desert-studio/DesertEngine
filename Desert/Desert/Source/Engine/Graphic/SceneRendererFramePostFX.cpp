#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SceneDepthResolveRenderer.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/ViewBudgetGate.hpp>

#include <mutex>
#include <Common/Core/Units.hpp>

#include <Common/Core/Profiler.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <atomic>
#include <chrono>
#include <format>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include <Engine/Graphic/SceneRendererFrame.hpp>

namespace Desert::Graphic
{
    void SceneRenderer::AddFrameJumpFlood( RDG::Builder& graph, FrameTextures& textures,
                                           const RDG::TextureRef display )
    {
        // An editor overlay: a reflection capture's face view shows the game scene only (as the editor
        // primitives, AddFrameEditorPrimitives).
        if ( m_ViewProfile.SceneCapture )
            return;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* jfa = UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
        if ( jfa == nullptr )
            return;
        // Whether anything is outlined is known once the mesh draws of this frame are gathered, before the
        // graph is built: it decides which nodes exist. Nothing outlined: no node at all, and the display image
        // stays exactly what the tonemap and the AA wrote.
        jfa->SetOutlineActive(
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
             UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->HasOutline() );
        if ( !jfa->Runs() || !jfa->Prepare() )
            return;
        // The input is the silhouette pass's target (AddFrameSilhouette), the output the view's: refs of this
        // graph, never an image the outline renderer asks another renderer for.
        const RDG::TextureRef mask = textures.Transients.SilhouetteMask;
        if ( !display.IsValid() || !mask.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] {}: no selection outline this frame: the {} is not in the graph",
                       m_ViewResources.GetName(), display.IsValid() ? "silhouette mask" : "view's output" );
            return;
        }
        const Common::ResultStr<RDG::TextureDesc> maskDesc = graph.GetTextureDesc( mask );
        if ( !maskDesc )
        {
            LOG_ERROR( "[SceneRenderer] {}: no selection outline this frame: {}", m_ViewResources.GetName(),
                       maskDesc.GetError() );
            return;
        }
        // The seeds live within this graph: transients of the mask's size, created only when a pass writes them.
        // Init writes seed[0]; the steps ping-pong between seed[0] and seed[1].
        const RDG::TextureDesc desc      = System::JumpFloodOutlineRenderer::GetSeedDesc( maskDesc.GetValue() );
        const uint32_t         stepCount = System::JumpFloodOutlineRenderer::GetStepCount( desc );
        const RDG::TextureRef  seeds[2]  = { graph.CreateTexture( desc, "JumpFlood.Seed0" ),
                                             graph.CreateTexture( desc, "JumpFlood.Seed1" ) };
        // Every seed texel is written by each pass (a fullscreen triangle); the clear is the old passes' default.
        const RDG::LoadOp clear = RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f );

        graph.AddPass(
             "PostFX: JumpFloodInit", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 jfa->DeclareInitBindings( pass, mask );
                 pass.ColorTarget( 0, seeds[0], clear );
             },
             [jfa]( RDG::PassContext& context ) -> Common::BoolResultStr { return jfa->RecordInit( context ); } );
        // Ping-pong propagation: one node per step, each sampling the seed the previous one wrote.
        for ( uint32_t step = 0; step < stepCount; ++step )
        {
            const RDG::TextureRef source = seeds[System::JumpFloodOutlineRenderer::GetStepSource( step )];
            const RDG::TextureRef target = seeds[1 - System::JumpFloodOutlineRenderer::GetStepSource( step )];
            graph.AddPass(
                 std::format( "PostFX: JumpFloodStep{}", step ), RDG::PassFlags::Raster,
                 [&]( RDG::PassBuilder& pass )
                 {
                     jfa->DeclareStepBindings( pass, source );
                     pass.ColorTarget( 0, target, clear );
                 },
                 [jfa, step, stepCount]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return jfa->RecordStep( context, step, stepCount ); } );
        }
        // The band over the display image (UE EPass::SelectionOutline): blended, so the old contents are loaded.
        const RDG::TextureRef seed  = seeds[System::JumpFloodOutlineRenderer::GetFinalSeedIndex( stepCount )];
        const float           gamma = m_DisplayGamma;
        graph.AddPass(
             "PostFX: JumpFloodFinal", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 // The composite's uniforms are the frame's, filled before the block naming them is declared.
                 jfa->FillFinalMaterial( gamma );
                 jfa->DeclareFinalBindings( pass, seed );
                 pass.ColorTarget( 0, display, RDG::LoadOp::Load() );
             },
             [jfa]( RDG::PassContext& context ) -> Common::BoolResultStr { return jfa->RecordFinal( context ); } );
    }

    RDG::TextureRef SceneRenderer::PrepareFrameAutoExposure( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( autoExp == nullptr || !autoExp->Prepare() )
            return {};
        // THE VIEW'S EYE ADAPTATION (UE FSceneViewState::EyeAdaptation): last frame's luminance is registered
        // into this graph - what the temporal node weights its samples with and what this frame adapts from.
        // This frame's is created by AddFrameAutoExposure, with the node that writes it.
        RDG::TextureRef previous = m_ViewState.EyeAdaptation().RegisterPrevious( graph );
        if ( !previous.IsValid() )
        {
            // No luminance yet (the view's first frame, a reset, a writer lost to a fault): the adaptation lands
            // on this frame's own measurement (UE EyeAdaptation_ForceTarget), so what it is blended with never
            // shows; the temporal node reads luminance 1 for this one frame.
            autoExp->OnTemporalHistoryReset();
            previous = textures.System.White;
        }
        return previous;
    }

    void SceneRenderer::AddFrameAutoExposure( RDG::Builder& graph, FrameTextures& textures,
                                              const std::vector<RDG::TextureRef>& sceneColor,
                                              const RDG::TextureRef               previous )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( autoExp == nullptr || sceneColor.empty() || !previous.IsValid() )
            return;
        // The histogram lives within this frame: a transient buffer of this graph, cleared, filled and resolved by
        // the three nodes below. Nothing reads it the next frame.
        const RDG::BufferRef histogram =
             graph.CreateBuffer( System::AutoExposureRenderer::GetHistogramDesc(), "AutoExposure.Histogram" );
        // Prepare refused a frame without a scene image, so the dispatch size is this frame's scene size.
        const RDG::TextureRef scene      = sceneColor.front();
        // The dispatch covers the texture the nodes read (the temporal output at the output extent, or the scene
        // colour at the render extent), never the scene target's image.
        const Common::ResultStr<RDG::TextureDesc> sceneDesc = graph.GetTextureDesc( scene );
        if ( !sceneDesc )
        {
            LOG_ERROR( "[SceneRenderer] auto exposure skipped this frame: {}", sceneDesc.GetError() );
            return;
        }
        const uint32_t width  = sceneDesc.GetValue().Size.Width;
        const uint32_t height = sceneDesc.GetValue().Size.Height;
        // This frame's adapted luminance: a transient every exposed pass after this one reads by ref
        // (FrameTransients::AdaptedLuminance; FrameAdaptedLuminance), extracted into the view for the next frame.
        // Created only together with its writer: a frame without these nodes leaves the ref invalid and its
        // readers take System.White.
        const RDG::TextureRef adapted        = m_ViewState.EyeAdaptation().CreateCurrent( graph );
        textures.Transients.AdaptedLuminance = adapted;

        // Clear and Histogram write the histogram, Average reads it: the graph orders the three and places their
        // barriers, and Average's write of the adapted image keeps all three alive.
        graph.AddPass(
             "PostFX: AutoExposureClear", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { autoExp->DeclareClearBindings( pass, histogram ); },
             [autoExp]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordClear( context ); } );
        // UE's metering mask (AutoExposureMeterMask), or its white dummy: every texel weighs 1.
        const RDG::TextureRef meterMaskImported =
             textures.Import( autoExp->MeterMaskImage(), "AutoExposure.MeterMask" );
        const RDG::TextureRef meterMask = meterMaskImported.IsValid() ? meterMaskImported : textures.System.White;
        graph.AddPass(
             "PostFX: AutoExposureHistogram", RDG::PassFlags::Compute, [&]( RDG::PassBuilder& pass )
             { autoExp->DeclareHistogramBindings( pass, scene, histogram, meterMask ); },
             [autoExp, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordHistogram( context, width, height ); } );
        // Next frame adapts from what this node writes: the view checks after Execute that it survived, and
        // otherwise the next adaptation snaps (EyeAdaptationHistory::EndFrame).
        m_ViewState.EyeAdaptation().SetWriterPass( graph.GetPassCount() );
        graph.AddPass(
             "PostFX: AutoExposureAverage", RDG::PassFlags::Compute, [&]( RDG::PassBuilder& pass )
             { autoExp->DeclareAverageBindings( pass, histogram, previous, adapted ); },
             [autoExp]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordAverage( context ); } );
    }

    RDG::TextureRef SceneRenderer::FrameAdaptedLuminance( FrameTextures& textures )
    {
        // Without an auto-exposure node the shaders read System.White; the manual exposure is used then (the auto
        // flag of ViewExposureOf is off), so the value is never read as a measurement.
        const RDG::TextureRef adapted = textures.Transients.AdaptedLuminance;
        return adapted.IsValid() ? adapted : textures.System.White;
    }

    RDG::TextureRef SceneRenderer::AddFrameColorGrading( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* luts = UNIQUE_GET_AS( System::CombineLUTsRenderer, m_RenderSystems["CombineLUTsSystem"] );
        if ( luts == nullptr )
            return {};
        const RDG::TextureRef lut = textures.Import( luts->GetLUTImage(), "ColorGrading.LUT" );
        if ( !lut.IsValid() || !luts->NeedsBake() )
            return lut;
        // The table outlives the frame like a history: if a fault removes this bake, what it holds is unknown and
        // the next frame bakes again.
        textures.MarkHistory( lut, [luts]() { luts->OnBakeLost(); } );
        const Graphic::FilmGradeParams grade = luts->GetGrade();
        const RDG::BufferRef           params =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( Graphic::FilmGradeParams ) }, "ColorGrading.Params" );
        graph.QueueBufferUpload( params, std::as_bytes( std::span( &grade, 1 ) ) );
        graph.AddPass(
             "PostFX: CombineLUTs", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { luts->DeclareBindings( pass, lut, params ); },
             [luts, grade]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return luts->Record( context, grade ); } );
        return lut;
    }

    void SceneRenderer::AddFrameBloom( RDG::Builder& graph, FrameTextures& textures,
                                       const std::vector<RDG::TextureRef>& sceneColor )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* bloom = UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] );
        if ( bloom == nullptr || sceneColor.empty() )
            return;
        const std::optional<RDG::TextureDesc> desc = bloom->GetChainDesc();
        if ( !desc )
            return;
        // A transient of this graph, sized from this frame's view; the tonemap reads its mip 0.
        const RDG::TextureRef chain = graph.CreateTexture( *desc, "Bloom" );
        graph.SetFaultDefault( chain, RDG::FaultDefault::Black ); // lost bloom adds nothing (RDG-FAULT1)
        const RDG::TextureRef scene = sceneColor.front();
        textures.Transients.Bloom   = chain;
        // The threshold is compared with the EXPOSED luminance (UE's bloom setup): the exposure is the tonemap's,
        // from the same adapted-luminance image.
        const RDG::TextureRef adapted = FrameAdaptedLuminance( textures );

        // Downsample: scene -> mip 0 (Karis + threshold), then mip i-1 -> mip i. One node per dispatch, each
        // declaring the one mip it samples and the one it writes.
        for ( uint32_t mip = 0; mip < desc->Mips; ++mip )
            graph.AddPass(
                 std::format( "PostFX: BloomDownsample{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 { bloom->DeclareDownsampleBindings( pass, scene, adapted, chain, mip ); },
                 [bloom, chainDesc = *desc, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return bloom->RecordDownsample( context, chainDesc, mip ); } );
        // Upsample (additive): mip i -> mip i-1, walking back to mip 0 (read-modify-write of the target mip).
        for ( uint32_t mip = desc->Mips - 1; mip >= 1; --mip )
            graph.AddPass(
                 std::format( "PostFX: BloomUpsample{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass ) { bloom->DeclareUpsampleBindings( pass, chain, mip ); },
                 [bloom, chainDesc = *desc, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return bloom->RecordUpsample( context, chainDesc, mip ); } );
    }

    void SceneRenderer::AddFrameLightShafts( RDG::Builder& graph, FrameTextures& textures,
                                             const std::vector<RDG::TextureRef>& sceneColor,
                                             const std::shared_ptr<FrameValues>& values )
    {
        // The sun's screen position is the frame's, known when the graph is built (the camera and the atmosphere
        // are); the shafts here and the lens flare after them read it from the frame's shared values, whether or
        // not the shafts run.
        const AtmosphereEnv& atmosphere = GetAtmosphere();
        if ( m_SceneInfo.ActiveCamera != nullptr && atmosphere.Valid )
        {
            const glm::mat4 viewProjection =
                 m_SceneInfo.ActiveCamera->GetProjectionMatrix() * m_SceneInfo.ActiveCamera->GetViewMatrix();
            values->Sun = ComputeSunScreen( viewProjection, atmosphere.SunDirection );
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* shafts = UNIQUE_GET_AS( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( shafts == nullptr || tonemap == nullptr || sceneColor.empty() )
            return;

        // The params and the tonemap's shaft intensity come from the same numbers that decide whether the nodes
        // exist, so a frame without shaft nodes is also a frame whose tonemap reads System.Black at intensity 0.
        const SunScreen& sun = values->Sun;
        shafts->SetParams( System::LightShaftRenderer::Params{
             .Enabled       = m_SunLightFx.LightShaftBloom,
             .BloomScale    = m_SunLightFx.BloomScale,
             .Threshold     = m_SunLightFx.BloomThreshold,
             .MaxBrightness = m_SunLightFx.BloomMaxBrightness,
             .BloomTint     = m_SunLightFx.BloomTint,
        } );
        const float intensity = m_SunLightFx.LightShaftBloom ? m_SunLightFx.BloomScale * sun.Fade : 0.0f;
        tonemap->SetLightShafts( intensity, m_SunLightFx.BloomTint );
        if ( !shafts->IsActive( sun.Fade ) )
            return;
        const std::optional<RDG::TextureDesc> desc = shafts->GetTargetDesc();
        if ( !desc )
            return;

        // Two half-resolution transients of this graph; mask -> ping, then ping -> pong, pong -> ping, ping ->
        // pong.
        const RDG::TextureRef scene = sceneColor.front();
        const RDG::TextureRef ping  = graph.CreateTexture( *desc, "LightShaft.Ping" );
        const RDG::TextureRef pong  = graph.CreateTexture( *desc, "LightShaft.Pong" );
        // Lost light shafts add nothing to the scene (RDG-FAULT1).
        graph.SetFaultDefault( ping, RDG::FaultDefault::Black );
        graph.SetFaultDefault( pong, RDG::FaultDefault::Black );
        const glm::vec2       sunUv = sun.Uv;

        graph.AddPass(
             "PostFX: LightShaftMask", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { shafts->DeclareMaskBindings( pass, scene, ping ); },
             [shafts, desc = *desc, sunUv]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return shafts->RecordMask( context, desc, sunUv ); } );
        // Radial blur ping-pong: one node per pass, each sampling the previous pass's target.
        RDG::TextureRef last = ping;
        for ( uint32_t blur = 0; blur < System::LightShaftRenderer::GetBlurPassCount(); ++blur )
        {
            const RDG::TextureRef source = blur % 2 == 0 ? ping : pong;
            const RDG::TextureRef target = blur % 2 == 0 ? pong : ping;
            graph.AddPass(
                 std::format( "PostFX: LightShaftBlur{}", blur ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass ) { shafts->DeclareBlurBindings( pass, source, target ); },
                 [shafts, desc = *desc, blur, sunUv]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return shafts->RecordBlur( context, desc, blur, sunUv ); } );
            last = target;
        }
        textures.Transients.LightShafts = last;
    }

    void SceneRenderer::AddFrameLensFlare( RDG::Builder& graph, FrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& sceneColor,
                                           const std::shared_ptr<FrameValues>& values )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* flare = UNIQUE_GET_AS( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( flare == nullptr || tonemap == nullptr || sceneColor.empty() )
            return;

        // The sun is the frame's (AddFrameLightShafts put it in the shared values when the graph was built).
        // The intensity is derived HERE, from the same two numbers that decide whether the nodes exist; a frame
        // without flare nodes has the tonemap read System.Black at intensity 0.
        const SunScreen& sun = values->Sun;
        flare->SetParams( m_LensFlare );
        const float intensity = m_LensFlare.Enabled ? LensFlareStrength( sun.Fade, m_LensFlare.Intensity ) : 0.0f;
        tonemap->SetLensFlare( intensity, m_LensFlareTint );
        if ( !flare->Prepare( sun.Fade ) )
            return;
        const std::optional<RDG::TextureDesc> sourceDesc = flare->GetSourceDesc();
        const std::optional<RDG::TextureDesc> flareDesc  = flare->GetFlareDesc();
        if ( !sourceDesc || !flareDesc )
            return;

        // Two transients of this graph: the half-resolution source chain and the quarter-resolution flare image.
        const RDG::TextureRef scene  = sceneColor.front();
        const RDG::TextureRef source = graph.CreateTexture( *sourceDesc, "LensFlare.Source" );
        const RDG::TextureRef image  = graph.CreateTexture( *flareDesc, "LensFlare" );
        graph.SetFaultDefault( image, RDG::FaultDefault::Black ); // a lost flare adds nothing (RDG-FAULT1)
        const glm::vec2       sunUv  = sun.Uv;
        // The bright pass compares its threshold in the EXPOSED image, as the bloom setup does: the exposure is
        // the tonemap's, from the same adapted-luminance image.
        const RDG::TextureRef adapted = FrameAdaptedLuminance( textures );

        // Bright pass: scene -> source mip 0 (thresholded), then mip i-1 -> mip i. One node per dispatch, each
        // declaring the one mip it samples and the one it writes.
        for ( uint32_t mip = 0; mip < sourceDesc->Mips; ++mip )
            graph.AddPass(
                 std::format( "PostFX: LensFlareBright{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 { flare->DeclareBrightPassBindings( pass, scene, adapted, source, mip ); },
                 [flare, desc = *sourceDesc, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return flare->RecordBrightPass( context, desc, mip ); } );
        // Features: every ghost reads the source mip its magnification picks, so the whole chain is sampled.
        graph.AddPass(
             "PostFX: LensFlareFeatures", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { flare->DeclareFeaturesBindings( pass, source, image ); },
             [flare, desc = *flareDesc, sunUv]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return flare->RecordFeatures( context, desc, sunUv ); } );
        textures.Transients.LensFlare = image;
    }

    RDG::TextureRef SceneRenderer::AddFrameTonemap( RDG::Builder& graph, FrameTextures& textures,
                                                    const RDG::TextureRef source, const RDG::TextureRef display,
                                                    const bool last )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( tonemap == nullptr )
            return {};
        const RDG::TextureRef avgLuminance = FrameAdaptedLuminance( textures );
        const RDG::TextureRef colorGrading = AddFrameColorGrading( graph, textures );
        // Bloom is this graph's transient when the chain ran; otherwise the graph's system black texture with
        // zero intensity (TonemapRenderer::GraphInputs), an explicit choice rather than a stale image.
        const bool bloomProduced = textures.Transients.Bloom.IsValid();
        // The light shafts and the lens flare follow the same rule.
        const bool                           shaftsProduced = textures.Transients.LightShafts.IsValid();
        const bool                           flareProduced  = textures.Transients.LensFlare.IsValid();
        System::TonemapRenderer::GraphInputs graphInputs{
             .Source              = source,
             .AvgLuminance        = avgLuminance,
             .ColorGradingLUT     = colorGrading,
             .Bloom               = bloomProduced ? textures.Transients.Bloom : textures.System.Black,
             .BloomProduced       = bloomProduced,
             .LightShafts         = shaftsProduced ? textures.Transients.LightShafts : textures.System.Black,
             .LightShaftsProduced = shaftsProduced,
             .LensFlare           = flareProduced ? textures.Transients.LensFlare : textures.System.Black,
             .LensFlareProduced   = flareProduced };
        if ( !graphInputs.Source.IsValid() || !graphInputs.AvgLuminance.IsValid() ||
             !graphInputs.ColorGradingLUT.IsValid() || !graphInputs.Bloom.IsValid() ||
             !graphInputs.LightShafts.IsValid() || !graphInputs.LensFlare.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] the tonemap is missing a graph input (no scene colour, no colour-grading "
                       "table, or System.Black / System.White is not in this graph); the tonemap pass is not "
                       "recorded this frame" );
            return {};
        }
        const Common::ResultStr<RDG::TextureDesc> displayDesc = graph.GetTextureDesc( display );
        if ( !displayDesc )
        {
            LOG_ERROR( "[SceneRenderer] {}: the tonemap pass is not recorded this frame: {}",
                       m_ViewResources.GetName(), displayDesc.GetError() );
            return {};
        }
        // The last pass of the chain writes the view's output; otherwise the tonemapped image lives within this
        // graph, at the display extent, for the spatial AA to read.
        const RDG::TextureRef output =
             last ? display
                  : graph.CreateTexture(
                         RDG::TextureDesc{ .Size   = { .Width  = displayDesc.GetValue().Size.Width,
                                                       .Height = displayDesc.GetValue().Size.Height },
                                           .Format = ViewTargetFormats::kTonemap },
                         "Tonemap" );
        graph.AddPass(
             "PostFX: Tonemap", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 tonemap->FillMaterial( graphInputs );
                 tonemap->DeclareBindings( pass, graphInputs );
                 // A fullscreen triangle writes every pixel: the old contents are not loaded.
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [tonemap]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return tonemap->Record( context ); } );
        return output;
    }

    void SceneRenderer::AddFrameFXAA( RDG::Builder& graph, const RDG::TextureRef input,
                                      const RDG::TextureRef output )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* fxaa = UNIQUE_GET_AS( System::FXAARenderer, m_RenderSystems["FXAASystem"] );
        if ( fxaa == nullptr || !input.IsValid() || !output.IsValid() )
            return;
        graph.AddPass(
             "PostFX: FXAA", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 fxaa->DeclareBindings( pass, input );
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [fxaa]( RDG::PassContext& context ) -> Common::BoolResultStr { return fxaa->Record( context ); } );
    }

    void SceneRenderer::AddFrameSMAA( RDG::Builder& graph, FrameTextures& textures, const RDG::TextureRef input,
                                      const RDG::TextureRef output )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* smaa = UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] );
        if ( smaa == nullptr || !input.IsValid() || !output.IsValid() )
            return;
        const Common::ResultStr<RDG::TextureDesc> inputDesc = graph.GetTextureDesc( input );
        if ( !inputDesc )
        {
            LOG_ERROR( "[SceneRenderer] {}: SMAA is not recorded this frame: {}", m_ViewResources.GetName(),
                       inputDesc.GetError() );
            return;
        }
        const std::optional<RDG::TextureDesc> desc = smaa->GetIntermediateDesc( inputDesc.GetValue() );
        if ( !desc )
            return;
        // Edges and weights live within this graph: transients sized from this frame's input.
        const RDG::TextureRef edges   = graph.CreateTexture( *desc, "SMAA.Edges" );
        const RDG::TextureRef weights = graph.CreateTexture( *desc, "SMAA.Weights" );
        const RDG::TextureRef area    = textures.Import( smaa->GetAreaTex(), "SMAA.AreaTex" );
        const RDG::TextureRef search  = textures.Import( smaa->GetSearchTex(), "SMAA.SearchTex" );
        // The edge shader discards pixels without an edge, so edges and weights are cleared (black) first.
        graph.AddPass(
             "PostFX: SMAAEdges", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 smaa->DeclareEdgesBindings( pass, input );
                 pass.ColorTarget( 0, edges, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [smaa]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return smaa->RecordEdges( context ); } );
        graph.AddPass(
             "PostFX: SMAAWeights", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 smaa->DeclareWeightsBindings( pass, edges, area, search );
                 pass.ColorTarget( 0, weights, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [smaa]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return smaa->RecordWeights( context ); } );
        graph.AddPass(
             "PostFX: SMAABlend", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 smaa->DeclareBlendBindings( pass, input, weights, edges, area );
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [smaa]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return smaa->RecordBlend( context ); } );
    }

    RDG::TextureRef SceneRenderer::AddFrameBackdropBlur( RDG::Builder& graph, FrameTextures& textures,
                                                         const std::vector<RDG::TextureRef>& sceneColor )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* backdrop = UNIQUE_GET_AS( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] );
        if ( backdrop == nullptr || sceneColor.empty() )
            return {};
        const std::optional<RDG::TextureDesc> desc = backdrop->GetPyramidDesc();
        if ( !desc )
            return {};
        // A transient of this graph, sized from this frame's view; the UI glass samples every mip of it.
        const RDG::TextureRef pyramid    = graph.CreateTexture( *desc, "BackdropBlur" );
        const RDG::TextureRef scene      = sceneColor.front();
        textures.Transients.BackdropBlur = pyramid;

        // Scene -> pyramid mip 0, then mip i-1 -> mip i: one node per dispatch, declaring the one mip it samples
        // and the one it writes.
        for ( uint32_t mip = 0; mip < desc->Mips; ++mip )
            graph.AddPass(
                 std::format( "UI: BackdropBlur{}", mip ), RDG::PassFlags::Compute, [&]( RDG::PassBuilder& pass )
                 { backdrop->DeclareDownsampleBindings( pass, scene, pyramid, mip ); },
                 [backdrop, pyramidDesc = *desc, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return backdrop->RecordDownsample( context, pyramidDesc, mip ); } );
        // The UI extension point samples the pyramid: the caller declares that read on the UI passes.
        return pyramid;
    }

    void SceneRenderer::AddFrameEditorPrimitives( RDG::Builder& graph, FrameTextures& textures,
                                                  const OverlayTargets& overlay, const RDG::TextureRef display )
    {
        // Outside a BeginScene/EndScene bracket there is no scene and no target to take the depth of. A reflection
        // capture's face view shows the game scene only, never an editor primitive.
        if ( !m_TargetFramebuffer || m_ViewProfile.SceneCapture )
            return;
        // What draws here this frame: the engine's debug lines (a developer build that shows them) and the passes
        // registered at the point. None: no node and no target.
        std::vector<SystemRasterPass> engine;
#if DESERT_DEV_INSTRUMENTS
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
             mesh != nullptr && mesh->DrawsDebugLines() )
            engine.push_back( mesh->DebugLinesPass() );
#endif // DESERT_DEV_INSTRUMENTS
        bool registered = false;
        if ( m_FrameExtensions )
            m_FrameExtensions->ForEachAt( RDG::ExtensionPoint::EditorPrimitives,
                                          [&registered]( const ExtensionPass& ) { registered = true; } );
        if ( engine.empty() && !registered )
            return;

        // THE DEPTH THEY TEST AGAINST: the scene's, at the display extent and unjittered (UE PopulateDepthPass
        // reads the temporally upsampled depth). After a resolve that is the overlay depth PopulateSceneDepth
        // filled; without one the scene target is at the display extent and never jittered: its depth, or at MSAA
        // the single-sample depth the scene depth resolve wrote.
        RDG::TextureRef sceneDepth;
        if ( overlay.IsValid() )
            sceneDepth = overlay.Depth;
        else if ( m_TargetFramebuffer->GetSpecification().Samples > 1 )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            auto* resolve =
                 UNIQUE_GET_AS( System::SceneDepthResolveRenderer, m_RenderSystems["SceneDepthResolveSystem"] );
            if ( resolve == nullptr || !resolve->IsReady() )
            {
                LOG_ERROR(
                     "[SceneRenderer] {}: no editor primitives this frame: the scene target has {} samples and "
                     "the scene depth resolve the editor primitives test against does not exist",
                     m_ViewResources.GetName(), m_TargetFramebuffer->GetSpecification().Samples );
                return;
            }
            sceneDepth = textures.Depth( resolve->GetFramebuffer(), "SceneDepthResolved" );
        }
        else
            sceneDepth = textures.Depth( m_TargetFramebuffer, "SceneColor" );
        if ( !sceneDepth.IsValid() || !display.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] {}: no editor primitives this frame: the {} is not in the graph",
                       m_ViewResources.GetName(), sceneDepth.IsValid() ? "view's output" : "scene depth" );
            return;
        }
        const Common::ResultStr<RDG::TextureDesc> displayDesc = graph.GetTextureDesc( display );
        if ( !displayDesc )
        {
            LOG_ERROR( "[SceneRenderer] {}: no editor primitives this frame: {}", m_ViewResources.GetName(),
                       displayDesc.GetError() );
            return;
        }

        if ( !m_PopulateEditorDepth )
            m_PopulateEditorDepth = std::make_unique<System::PopulateSceneDepthRenderer>(
                 EditorPrimitivesLayout(), "Editor: PopulateDepth" );
        if ( !m_EditorPrimitivesComposite )
            m_EditorPrimitivesComposite = std::make_unique<System::EditorPrimitivesCompositeRenderer>();
        if ( const Common::BoolResultStr prepared = m_PopulateEditorDepth->Prepare(); !prepared )
        {
            LOG_ERROR( "[SceneRenderer] {}: no editor primitives this frame: {}", m_ViewResources.GetName(),
                       prepared.GetError() );
            return;
        }
        if ( const Common::BoolResultStr prepared = m_EditorPrimitivesComposite->Prepare(); !prepared )
        {
            LOG_ERROR( "[SceneRenderer] {}: no editor primitives this frame: {}", m_ViewResources.GetName(),
                       prepared.GetError() );
            return;
        }

        // UE SceneTextures.cpp:817-822: Editor.PrimitivesColor (cleared transparent) and Editor.PrimitivesDepth,
        // at the display extent, r.MSAA.CompositingSampleCount samples.
        EditorPrimitiveTargets editor;
        RDG::TextureDesc       desc;
        desc.Size    = RDG::Extent3D{ displayDesc.GetValue().Size.Width, displayDesc.GetValue().Size.Height, 1 };
        desc.Samples = ViewTargetFormats::kEditorPrimitiveSamples;
        desc.Format  = ViewTargetFormats::kEditorPrimitiveColor;
        editor.Color = graph.CreateTexture( desc, "Editor.PrimitivesColor" );
        desc.Format  = ViewTargetFormats::kEditorPrimitiveDepth;
        editor.Depth = graph.CreateTexture( desc, "Editor.PrimitivesDepth" );

        const System::PopulateSceneDepthRenderer* populate = m_PopulateEditorDepth.get();
        graph.AddPass(
             "Editor: PopulateDepth", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 populate->DeclareBindings( pass, sceneDepth );
                 pass.ColorTarget( 0, editor.Color, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                 pass.DepthTarget( editor.Depth, RDG::LoadOp::ClearDepth( Core::kDepthClear ), /*write*/ true );
             },
             [populate]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return populate->Record( context ); } );

        // The engine's debug lines first, then the registered passes in registration order: all LOAD the targets.
        AddSystemRasters( graph, textures, engine, false, {}, editor );
        AddExtensionPoint( graph, textures, RDG::ExtensionPoint::EditorPrimitives, {}, editor );

        const System::EditorPrimitivesCompositeRenderer* composite = m_EditorPrimitivesComposite.get();
        const float                                      gamma     = m_DisplayGamma;
        graph.AddPass(
             "Editor: CompositeEditorPrimitives", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 composite->DeclareBindings( pass, editor.Color, editor.Depth, sceneDepth, gamma );
                 // Blended over what the tonemap / AA wrote: the old contents are loaded.
                 pass.ColorTarget( 0, display, RDG::LoadOp::Load() );
             },
             [composite]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return composite->Record( context ); } );
    }
} // namespace Desert::Graphic

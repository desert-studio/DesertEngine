#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>
#include <Engine/Graphic/PostProcessing/LocalExposureRules.hpp>
#include <Engine/Graphic/Systems/Scene/PostProcessing/LocalExposureRenderer.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/ViewBudgetGate.hpp>

#include <mutex>
#include <Common/Core/Units.hpp>

#include <Common/Core/Profiler.hpp>

#include <glm/glm.hpp>

#include <algorithm>
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
    RDG::TextureRef SceneRenderer::AddFrameJumpFlood( RDG::Builder& graph, FrameTextures& textures,
                                                      const RDG::TextureRef scene )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* jfa = UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
        if ( jfa == nullptr || !scene.IsValid() )
            return {};
        // Whether anything is outlined is known once the mesh draws of this frame are gathered, before the
        // graph is built: it decides which nodes exist.
        jfa->SetOutlineActive(
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
             UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->HasOutline() );
        if ( !jfa->Prepare() )
            return {};
        // The seeds live within this graph: transients of the scene's size, created only when a pass writes them.
        // Init writes seed[0]; the steps ping-pong between seed[0] and seed[1].
        RDG::TextureRef seeds[2];
        if ( jfa->RunsInit() )
        {
            const std::optional<RDG::TextureDesc> desc = jfa->GetSeedDesc();
            if ( !desc )
                return {};
            seeds[0] = graph.CreateTexture( *desc, "JumpFlood.Seed0" );
            if ( jfa->RunsSteps() )
                seeds[1] = graph.CreateTexture( *desc, "JumpFlood.Seed1" );
        }
        // The old passes cleared every target (RenderPassSpecification's default clear, black); kept.
        const RDG::LoadOp clear = RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f );

        if ( jfa->RunsInit() )
        {
            const RDG::TextureRef mask = textures.Import( jfa->GetMaskImage(), "JumpFlood.Mask" );
            graph.AddPass(
                 "PostFX: JumpFloodInit", RDG::PassFlags::Raster,
                 [&]( RDG::PassBuilder& pass )
                 {
                     jfa->DeclareInitBindings( pass, mask );
                     pass.ColorTarget( 0, seeds[0], clear );
                 },
                 [jfa]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return jfa->RecordInit( context ); } );
        }
        // Ping-pong propagation: one node per step, each sampling the seed the previous one wrote.
        if ( jfa->RunsSteps() )
            for ( uint32_t step = 0; step < jfa->GetStepCount(); ++step )
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
                     [jfa, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return jfa->RecordStep( context, step ); } );
            }
        // The composite runs on every frame, outlined or not: it is what hands the scene colour to the tonemap
        // (TonemapRenderer::Inputs::Source is GetOutputImage()). Without Init no seed exists this frame: the
        // composite samples the engine's black texture and passes the scene through (width 0).
        const RDG::TextureRef output = textures.Import( jfa->GetOutputImage(), "Tonemap.Source" );
        const RDG::TextureRef seed   = jfa->RunsInit() ? seeds[jfa->GetFinalSeedIndex()] : textures.System.Black;
        graph.AddPass(
             "PostFX: JumpFloodFinal", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 // The composite's uniforms are the frame's, filled before the block naming them is declared.
                 jfa->FillFinalMaterial();
                 jfa->DeclareFinalBindings( pass, seed, scene );
                 pass.ColorTarget( 0, output, clear );
             },
             [jfa]( RDG::PassContext& context ) -> Common::BoolResultStr { return jfa->RecordFinal( context ); } );
        return output;
    }

    RDG::TextureRef SceneRenderer::PrepareFrameAutoExposure( FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( autoExp == nullptr )
            return {};
        // Prepare picks which 1x1 image this frame writes, which the graph must know to import it and the tonemap
        // to sample it: a build-time decision. After it, GetPreviousLuminanceImage() is last frame's adapted
        // luminance - what the temporal node weights its samples with and what this frame adapts from.
        if ( !autoExp->Prepare() )
            return {};
        // The tonemap samples the luminance this frame writes; that image is known when the graph is built.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetAutoExposureImage( autoExp->GetAdaptedLuminanceImage() );
        return textures.Import( autoExp->GetPreviousLuminanceImage(), "AutoExposure.Previous" );
    }

    void SceneRenderer::AddFrameAutoExposure( RDG::Builder& graph, FrameTextures& textures,
                                              const std::vector<RDG::TextureRef>& sceneColor,
                                              const RDG::TextureRef               previous )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( autoExp == nullptr || sceneColor.empty() || !previous.IsValid() )
            return;
        const RDG::TextureRef adapted =
             textures.Import( autoExp->GetAdaptedLuminanceImage(), "AutoExposure.Adapted" );
        // Next frame adapts from this luminance: if a fault removes its writer, the next adaptation snaps.
        if ( adapted.IsValid() )
            textures.MarkHistory( adapted, [autoExp]() { autoExp->OnTemporalHistoryReset(); } );
        // The histogram lives within this frame: a transient buffer of this graph, cleared, filled and resolved by
        // the three nodes below. Nothing reads it the next frame.
        const RDG::BufferRef histogram =
             graph.CreateBuffer( System::AutoExposureRenderer::GetHistogramDesc(), "AutoExposure.Histogram" );
        // The adapted luminance for the CPU (ScenePreExposure's source), read FramesInFlight frames later.
        const RDG::BufferRef readback = autoExp->ImportReadback( graph );
        if ( !readback.IsValid() )
            return;
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

        // Clear and Histogram write the histogram, Average reads it: the graph orders the three and places their
        // barriers, and Average's write of the adapted image keeps all three alive.
        graph.AddPass(
             "PostFX: AutoExposureClear", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { autoExp->DeclareClearBindings( pass, histogram ); },
             [autoExp]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordClear( context ); } );
        graph.AddPass(
             "PostFX: AutoExposureHistogram", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { autoExp->DeclareHistogramBindings( pass, scene, histogram ); },
             [autoExp, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordHistogram( context, width, height ); } );
        graph.AddPass(
             "PostFX: AutoExposureAverage", RDG::PassFlags::Compute, [&]( RDG::PassBuilder& pass )
             { autoExp->DeclareAverageBindings( pass, histogram, previous, adapted, readback ); },
             [autoExp]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordAverage( context ); } );
    }

    void SceneRenderer::AddFrameLocalExposure( RDG::Builder& graph, FrameTextures& textures,
                                               const std::vector<RDG::TextureRef>& sceneColor )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* le = UNIQUE_GET_AS( System::LocalExposureRenderer, m_RenderSystems["LocalExposureSystem"] );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( tonemap == nullptr )
            return;
        const bool visualize = m_DebugView.DeferredDebug == DeferredDebugMode::LocalExposure;
        System::TonemapRenderer::LocalExposureFrame frame{ .Visualize = visualize };
        tonemap->SetLocalExposure( frame );
        // Identity settings: every pixel's local exposure is exactly 1 and nothing runs (UE PostProcessing.cpp:799).
        if ( le == nullptr || !le->IsReady() || sceneColor.empty() || IsLocalExposureIdentity( le->GetParams() ) )
            return;
        const RDG::TextureRef                      scene     = sceneColor.front();
        const Common::ResultStr<RDG::TextureDesc> sceneDesc = graph.GetTextureDesc( scene );
        if ( !sceneDesc )
        {
            LOG_ERROR( "[SceneRenderer] local exposure skipped this frame: {}", sceneDesc.GetError() );
            return;
        }
        const uint32_t width  = sceneDesc.GetValue().Size.Width;
        const uint32_t height = sceneDesc.GetValue().Size.Height;

        const RDG::TextureRef grid =
             graph.CreateTexture( System::LocalExposureRenderer::GetGridDesc( width, height ), "LocalExposure.Grid" );
        const RDG::TextureDesc logLumDesc = System::LocalExposureRenderer::GetLogLumDesc( width, height );
        const RDG::TextureRef  logLum     = graph.CreateTexture( logLumDesc, "LocalExposure.LogLuminance" );
        graph.AddPass(
             "PostFX: LocalExposureSetup", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { le->DeclareSetupBindings( pass, scene, grid, logLum ); },
             [le, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return le->RecordSetup( context, width, height ); } );

        // The blur runs only when the base layer uses it (UE PostProcessing.cpp:870); otherwise the base is the grid
        // alone and the blurred input is the graph's black (read with weight 0, or as an empty cell's fallback 0).
        RDG::TextureRef blurred = textures.System.Black;
        if ( LocalExposureNeedsBlur( le->GetParams() ) )
        {
            const uint32_t blurW  = logLumDesc.Size.Width;
            const uint32_t blurH  = logLumDesc.Size.Height;
            const float    radius = LocalExposureBlurRadius( blurW, le->GetParams().BlurKernelSizePercent );
            const RDG::TextureRef blurX = graph.CreateTexture( logLumDesc, "LocalExposure.BlurH" );
            blurred                     = graph.CreateTexture( logLumDesc, "LocalExposure.Blurred" );
            graph.AddPass(
                 "PostFX: LocalExposureBlurH", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass ) { le->DeclareBlurBindings( pass, logLum, blurX ); },
                 [le, radius, blurW, blurH]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return le->RecordBlur( context, true, radius, blurW, blurH ); } );
            graph.AddPass(
                 "PostFX: LocalExposureBlurV", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass ) { le->DeclareBlurBindings( pass, blurX, blurred ); },
                 [le, radius, blurW, blurH]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return le->RecordBlur( context, false, radius, blurW, blurH ); } );
        }
        textures.Transients.LocalExposureGrid   = grid;
        textures.Transients.BlurredLogLuminance = blurred;

        frame.Produced      = true;
        frame.Params        = le->GetParams();
        frame.LogMiddleGrey = le->GetLogMiddleGrey();
        frame.SliceLogMin   = le->GetSliceLogMin();
        frame.GridUVScale   = LocalExposureGridUVScale( width, height );
        frame.BlurUVScale   = LocalExposureBlurUVScale( width, height );
        tonemap->SetLocalExposure( frame );
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
        // The adapted luminance the composite exposes with (the same import AddFrameTonemap makes), so the
        // threshold under auto-exposure is compared after exposure; white when there is no auto-exposure node.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        const RDG::TextureRef adaptedImport =
             autoExp ? textures.Import( autoExp->GetAdaptedLuminanceImage(), "AutoExposure.Adapted" )
                     : RDG::TextureRef{};
        const RDG::TextureRef adapted = adaptedImport.IsValid() ? adaptedImport : textures.System.White;
        // Local exposure (UE r.Bloom.ApplyLocalExposure): AddFrameLocalExposure ran first and handed the tonemap
        // this frame's numbers; the bloom's mip 0 applies the same ones, so the bloom is exposed like its source.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( tonemap != nullptr )
            bloom->SetLocalExposure( tonemap->GetLocalExposure(), tonemap->GetExposure() );
        const RDG::TextureRef leGrid    = textures.Transients.LocalExposureGrid.IsValid()
                                               ? textures.Transients.LocalExposureGrid
                                               : textures.System.Black3D;
        const RDG::TextureRef leBlurred = textures.Transients.BlurredLogLuminance.IsValid()
                                               ? textures.Transients.BlurredLogLuminance
                                               : textures.System.Black;

        // Downsample: scene -> mip 0 (Karis + threshold), then mip i-1 -> mip i. One node per dispatch, each
        // declaring the one mip it samples and the one it writes.
        for ( uint32_t mip = 0; mip < desc->Mips; ++mip )
            graph.AddPass(
                 std::format( "PostFX: BloomDownsample{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 { bloom->DeclareDownsampleBindings( pass, scene, chain, adapted, leGrid, leBlurred, mip ); },
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
        // The bright pass thresholds the STORED scene colour, which is radiance x ViewFrame::PreExposure: the
        // authored threshold (normalised to the exposure in OnUpdate) and the energy cap are scaled into that
        // same space here.
        System::LensFlareRenderer::Params params = m_LensFlare;
        if ( const ViewFrame* view = GetViewFrame() )
        {
            params.Threshold *= view->PreExposure;
            params.MaxBrightness *= view->PreExposure;
        }
        flare->SetParams( params );
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

        // Bright pass: scene -> source mip 0 (thresholded), then mip i-1 -> mip i. One node per dispatch, each
        // declaring the one mip it samples and the one it writes.
        for ( uint32_t mip = 0; mip < sourceDesc->Mips; ++mip )
            graph.AddPass(
                 std::format( "PostFX: LensFlareBright{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass ) { flare->DeclareBrightPassBindings( pass, scene, source, mip ); },
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

    void SceneRenderer::AddFrameTonemap( RDG::Builder& graph, FrameTextures& textures,
                                         const RDG::TextureRef source )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( tonemap == nullptr )
            return;
        const System::TonemapRenderer::Inputs inputs = tonemap->GetInputs();
        // Without an auto-exposure node the shader's AverageLuminanceTexture reads System.White; the manual exposure is
        // used then (the auto flag is off), so the value is never read as a measurement.
        const RDG::TextureRef adapted      = textures.Import( inputs.AutoExposure, "AutoExposure.Adapted" );
        const RDG::TextureRef avgLuminance = adapted.IsValid() ? adapted : textures.System.White;
        // Bloom is this graph's transient when the chain ran; otherwise the graph's system black texture with
        // zero intensity (TonemapRenderer::GraphInputs), an explicit choice rather than a stale image.
        const bool bloomProduced = textures.Transients.Bloom.IsValid();
        // The light shafts and the lens flare follow the same rule.
        const bool                           shaftsProduced = textures.Transients.LightShafts.IsValid();
        const bool                           flareProduced  = textures.Transients.LensFlare.IsValid();
        System::TonemapRenderer::GraphInputs graphInputs{
             .Source              = source,
             .AvgLuminance        = avgLuminance,
             .Bloom               = bloomProduced ? textures.Transients.Bloom : textures.System.Black,
             .BloomProduced       = bloomProduced,
             .LightShafts         = shaftsProduced ? textures.Transients.LightShafts : textures.System.Black,
             .LightShaftsProduced = shaftsProduced,
             .LensFlare           = flareProduced ? textures.Transients.LensFlare : textures.System.Black,
             .LensFlareProduced   = flareProduced,
             .LocalExposureGrid   = textures.Transients.LocalExposureGrid.IsValid() ? textures.Transients.LocalExposureGrid
                                                                                    : textures.System.Black3D,
             .BlurredLogLuminance = textures.Transients.BlurredLogLuminance.IsValid()
                                         ? textures.Transients.BlurredLogLuminance
                                         : textures.System.Black };
        if ( !graphInputs.Source.IsValid() || !graphInputs.AvgLuminance.IsValid() ||
             !graphInputs.Bloom.IsValid() || !graphInputs.LightShafts.IsValid() ||
             !graphInputs.LensFlare.IsValid() || !graphInputs.LocalExposureGrid.IsValid() ||
             !graphInputs.BlurredLogLuminance.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] the tonemap is missing a graph input (no scene colour, or System.Black / "
                       "System.White is not in this graph); the tonemap pass is not recorded this frame" );
            return;
        }
        const RDG::TextureRef output = textures.Import( tonemap->GetOutputImage(), "Tonemap" );
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
    }

    void SceneRenderer::AddFrameFXAA( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* fxaa = UNIQUE_GET_AS( System::FXAARenderer, m_RenderSystems["FXAASystem"] );
        if ( fxaa == nullptr )
            return;
        const RDG::TextureRef input  = textures.Import( fxaa->GetInputImage(), "Tonemap" );
        const RDG::TextureRef output = textures.Import( fxaa->GetOutputImage(), "FXAA" );
        graph.AddPass(
             "PostFX: FXAA", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 fxaa->DeclareBindings( pass, input );
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [fxaa]( RDG::PassContext& context ) -> Common::BoolResultStr { return fxaa->Record( context ); } );
    }

    void SceneRenderer::AddFrameSMAA( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* smaa = UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] );
        if ( smaa == nullptr )
            return;
        const std::optional<RDG::TextureDesc> desc = smaa->GetIntermediateDesc();
        if ( !desc )
            return;
        const RDG::TextureRef input = textures.Import( smaa->GetInputImage(), "Tonemap" );
        // Edges and weights live within this graph: transients sized from this frame's input.
        const RDG::TextureRef edges   = graph.CreateTexture( *desc, "SMAA.Edges" );
        const RDG::TextureRef weights = graph.CreateTexture( *desc, "SMAA.Weights" );
        const RDG::TextureRef area    = textures.Import( smaa->GetAreaTex(), "SMAA.AreaTex" );
        const RDG::TextureRef search  = textures.Import( smaa->GetSearchTex(), "SMAA.SearchTex" );
        const RDG::TextureRef output  = textures.Import( smaa->GetOutputImage(), "SMAA" );
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
} // namespace Desert::Graphic

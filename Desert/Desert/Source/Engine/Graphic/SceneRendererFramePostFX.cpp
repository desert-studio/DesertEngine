#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RenderPhaseRegistry.hpp>
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
    namespace
    {
        void ReadEach( RDG::PassBuilder& pass, const std::vector<RDG::TextureRef>& refs, RDG::Access access )
        {
            for ( const RDG::TextureRef ref : refs )
                pass.Read( ref, access );
        }
    } // namespace

    void SceneRenderer::AddFrameJumpFlood( RDG::Builder& graph, FrameTextures& textures )
    {
        auto* jfa = UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
        if ( !jfa )
            return;
        // Whether anything is outlined is known once the mesh draws of this frame are gathered, before the
        // graph is built: it decides which nodes exist.
        jfa->SetOutlineActive(
             UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->HasOutline() );
        if ( !jfa->Prepare() )
            return;
        // The seeds live within this graph: transients of the scene's size, created only when a pass writes them.
        // Init writes seed[0]; the steps ping-pong between seed[0] and seed[1].
        RDG::TextureRef seeds[2];
        if ( jfa->RunsInit() )
        {
            const std::optional<RDG::TextureDesc> desc = jfa->GetSeedDesc();
            if ( !desc )
                return;
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
                     pass.Read( mask, RDG::Access::SampledGraphics );
                     pass.ColorTarget( 0, seeds[0], clear );
                 },
                 [jfa, mask]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return jfa->RecordInit( context, mask ); } );
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
                         pass.Read( source, RDG::Access::SampledGraphics );
                         pass.ColorTarget( 0, target, clear );
                     },
                     [jfa, step, source]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return jfa->RecordStep( context, step, source ); } );
            }
        // The composite runs on every frame, outlined or not: it is what hands the scene colour to the tonemap
        // (TonemapRenderer::Inputs::Source is GetOutputImage()). Without Init no seed exists this frame: the
        // composite samples the engine's black texture and passes the scene through (width 0).
        const RDG::TextureRef scene  = textures.Import( jfa->GetSceneColorImage(), "JumpFlood.Scene" );
        const RDG::TextureRef output = textures.Import( jfa->GetOutputImage(), "Tonemap.Source" );
        const RDG::TextureRef seed   = jfa->RunsInit() ? seeds[jfa->GetFinalSeedIndex()] : textures.System.Black;
        graph.AddPass(
             "PostFX: JumpFloodFinal", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( scene, RDG::Access::SampledGraphics );
                 pass.Read( seed, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, output, clear );
             },
             [jfa, seed, scene]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return jfa->RecordFinal( context, seed, scene ); } );
    }

    void SceneRenderer::AddFrameAutoExposure( RDG::Builder& graph, FrameTextures& textures,
                                              const std::vector<RDG::TextureRef>& sceneColor )
    {
        auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( !autoExp || sceneColor.empty() )
            return;
        // Prepare picks which 1x1 image this frame writes, which the graph must know to import it and the tonemap
        // to sample it: a build-time decision.
        if ( !autoExp->Prepare() )
            return;
        // The tonemap samples the luminance this frame writes; that image is known when the graph is built.
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetAutoExposureImage( autoExp->GetAdaptedLuminanceImage() );
        const RDG::TextureRef previous =
             textures.Import( autoExp->GetPreviousLuminanceImage(), "AutoExposure.Previous" );
        const RDG::TextureRef adapted =
             textures.Import( autoExp->GetAdaptedLuminanceImage(), "AutoExposure.Adapted" );
        // The histogram lives within this frame: a transient buffer of this graph, cleared, filled and resolved by
        // the three nodes below. Nothing reads it the next frame.
        const RDG::BufferRef histogram =
             graph.CreateBuffer( System::AutoExposureRenderer::GetHistogramDesc(), "AutoExposure.Histogram" );
        // Prepare refused a frame without a scene image, so the dispatch size is this frame's scene size.
        const RDG::TextureRef scene      = sceneColor.front();
        const auto            sceneImage = autoExp->GetSceneColorImage();
        const uint32_t        width      = sceneImage->GetWidth();
        const uint32_t        height     = sceneImage->GetHeight();

        // Clear and Histogram write the histogram, Average reads it: the graph orders the three and places their
        // barriers, and Average's write of the adapted image keeps all three alive.
        graph.AddPass(
             "PostFX: AutoExposureClear", RDG::PassFlags::Compute,
             [histogram]( RDG::PassBuilder& pass ) { pass.Write( histogram, RDG::Access::StorageWrite ); },
             [autoExp, histogram]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordClear( context, histogram ); } );
        graph.AddPass(
             "PostFX: AutoExposureHistogram", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( scene, RDG::Access::SampledCompute );
                 pass.Write( histogram, RDG::Access::StorageWrite );
             },
             [autoExp, scene, histogram, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordHistogram( context, scene, histogram, width, height ); } );
        graph.AddPass(
             "PostFX: AutoExposureAverage", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( histogram, RDG::Access::StorageRead );
                 pass.Read( previous, RDG::Access::SampledCompute );
                 pass.Write( adapted, RDG::Access::StorageWrite );
             },
             [autoExp, histogram, previous, adapted]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return autoExp->RecordAverage( context, histogram, previous, adapted ); } );
    }

    void SceneRenderer::AddFrameBloom( RDG::Builder& graph, FrameTextures& textures,
                                       const std::vector<RDG::TextureRef>& sceneColor )
    {
        auto* bloom = UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] );
        if ( !bloom || sceneColor.empty() )
            return;
        const std::optional<RDG::TextureDesc> desc = bloom->GetChainDesc();
        if ( !desc )
            return;
        // A transient of this graph, sized from this frame's view; the tonemap reads its mip 0.
        const RDG::TextureRef chain = graph.CreateTexture( *desc, "Bloom" );
        const RDG::TextureRef scene = sceneColor.front();
        textures.Transients.Bloom   = chain;

        // Downsample: scene -> mip 0 (Karis + threshold), then mip i-1 -> mip i. One node per dispatch, each
        // declaring the one mip it samples and the one it writes.
        for ( uint32_t mip = 0; mip < desc->Mips; ++mip )
            graph.AddPass(
                 std::format( "PostFX: BloomDownsample{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     if ( mip == 0 )
                         pass.Read( scene, RDG::Access::SampledCompute );
                     else
                         pass.Read( chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ) );
                     pass.Write( chain, RDG::Access::StorageWrite, RDG::SubresourceRange::Mip( mip ) );
                 },
                 [bloom, scene, chain, chainDesc = *desc, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return bloom->RecordDownsample( context, scene, chain, chainDesc, mip ); } );
        // Upsample (additive): mip i -> mip i-1, walking back to mip 0 (read-modify-write of the target mip).
        for ( uint32_t mip = desc->Mips - 1; mip >= 1; --mip )
            graph.AddPass(
                 std::format( "PostFX: BloomUpsample{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Read( chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip ) );
                     pass.Write( chain, RDG::Access::StorageWrite, RDG::SubresourceRange::Mip( mip - 1 ) );
                 },
                 [bloom, chain, chainDesc = *desc, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return bloom->RecordUpsample( context, chain, chainDesc, mip ); } );
    }

    void SceneRenderer::AddFrameLightShafts( RDG::Builder& graph, FrameTextures& textures,
                                             const std::vector<RDG::TextureRef>& sceneColor,
                                             const std::shared_ptr<FrameValues>& values )
    {
        // The sun's screen position is the frame's, known when the graph is built (the camera and the atmosphere
        // are); the shafts here and the lens flare after them read it from the frame's shared values, whether or
        // not the shafts run.
        const AtmosphereEnv& atmosphere = GetAtmosphere();
        if ( m_SceneInfo.ActiveCamera && atmosphere.Valid )
        {
            const glm::mat4 viewProjection =
                 m_SceneInfo.ActiveCamera->GetProjectionMatrix() * m_SceneInfo.ActiveCamera->GetViewMatrix();
            values->Sun = ComputeSunScreen( viewProjection, atmosphere.SunDirection );
        }

        auto* shafts  = UNIQUE_GET_AS( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( !shafts || !tonemap || !shafts->Prepare() )
            return;
        tonemap->SetLightShaftImage( shafts->GetShaftImage() );
        const RDG::TextureRef ping = textures.Import( shafts->GetPingImage(), "LightShaft.Ping" );

        // Mask: scene HDR -> ping, toward the sun computed above.
        graph.AddPass(
             "PostFX: LightShaftMask", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadEach( pass, sceneColor, RDG::Access::SampledCompute );
                 pass.Write( ping, RDG::Access::StorageWrite );
             },
             [this, shafts, tonemap, values]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 const SunScreen& sun = values->Sun;

                 shafts->SetParams( System::LightShaftRenderer::Params{
                      .Enabled       = m_SunLightFx.LightShaftBloom,
                      .BloomScale    = m_SunLightFx.BloomScale,
                      .Threshold     = m_SunLightFx.BloomThreshold,
                      .MaxBrightness = m_SunLightFx.BloomMaxBrightness,
                      .BloomTint     = m_SunLightFx.BloomTint,
                 } );
                 shafts->RecordMask( sun.Uv, sun.Fade );

                 const float intensity = m_SunLightFx.LightShaftBloom ? m_SunLightFx.BloomScale * sun.Fade : 0.0f;
                 tonemap->SetLightShafts( intensity, m_SunLightFx.BloomTint );
                 return BOOLSUCCESS;
             } );
        // Radial blur ping-pong: one node per pass, each sampling the previous pass's target.
        for ( uint32_t blur = 0; blur < System::LightShaftRenderer::GetBlurPassCount(); ++blur )
        {
            const RDG::TextureRef source = textures.Import(
                 shafts->GetBlurSource( blur ), blur % 2 == 0 ? "LightShaft.Ping" : "LightShaft.Pong" );
            const RDG::TextureRef target = textures.Import(
                 shafts->GetBlurTarget( blur ), blur % 2 == 0 ? "LightShaft.Pong" : "LightShaft.Ping" );
            graph.AddPass(
                 std::format( "PostFX: LightShaftBlur{}", blur ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Read( source, RDG::Access::SampledCompute );
                     pass.Write( target, RDG::Access::StorageWrite );
                 },
                 [shafts, values, blur]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     shafts->RecordBlur( blur, values->Sun.Uv, values->Sun.Fade );
                     return BOOLSUCCESS;
                 } );
        }
    }

    void SceneRenderer::AddFrameLensFlare( RDG::Builder& graph, FrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& sceneColor,
                                           const std::shared_ptr<FrameValues>& values )
    {
        auto* flare   = UNIQUE_GET_AS( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( !flare || !tonemap )
            return;
        tonemap->SetLensFlareImage( flare->GetFlareImage() );

        // The sun is the frame's (AddFrameLightShafts put it in the shared values when the graph was built).
        // The intensity is derived HERE, from the same two numbers that decide whether the dispatches run, so a
        // zero intensity and a skipped dispatch can never disagree — the bloom image's contract.
        const SunScreen& sun = values->Sun;
        flare->SetParams( m_LensFlare );
        const float intensity = m_LensFlare.Enabled ? LensFlareStrength( sun.Fade, m_LensFlare.Intensity ) : 0.0f;
        tonemap->SetLensFlare( intensity, m_LensFlareTint );
        if ( !flare->Prepare( sun.Uv, sun.Fade ) )
            return;

        const RDG::TextureRef source = textures.Import( flare->GetSourceImage(), "LensFlare.Source" );
        const RDG::TextureRef image  = textures.Import( flare->GetFlareImage(), "LensFlare" );
        const uint32_t        mips   = flare->GetSourceMipLevels();

        // Bright pass: scene -> source mip 0 (thresholded), then mip i-1 -> mip i. One node per dispatch, each
        // declaring the one mip it samples and the one it writes.
        for ( uint32_t mip = 0; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "PostFX: LensFlareBright{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     if ( mip == 0 )
                         ReadEach( pass, sceneColor, RDG::Access::SampledCompute );
                     else
                         pass.Read( source, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ) );
                     pass.Write( source, RDG::Access::StorageWrite, RDG::SubresourceRange::Mip( mip ) );
                 },
                 [flare, mip]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     flare->RecordBrightPass( mip );
                     return BOOLSUCCESS;
                 } );
        // Features: every ghost reads the source mip its magnification picks, so the whole chain is sampled.
        graph.AddPass(
             "PostFX: LensFlareFeatures", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( source, RDG::Access::SampledCompute );
                 pass.Write( image, RDG::Access::StorageWrite );
             },
             [flare]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 flare->RecordFeatures();
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameTonemap( RDG::Builder& graph, FrameTextures& textures )
    {
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( !tonemap )
            return;
        const System::TonemapRenderer::Inputs inputs = tonemap->GetInputs();
        const std::vector<RDG::TextureRef>    reads  = {
             textures.Import( inputs.Source, "Tonemap.Source" ),
             textures.Import( inputs.AutoExposure, "AutoExposure.Adapted" ),
             textures.Import( inputs.LightShafts, "LightShaft.Pong" ),
             textures.Import( inputs.LensFlare, "LensFlare" ),
        };
        // Bloom is this graph's transient when the chain ran; otherwise the graph's system black texture with
        // zero intensity (TonemapRenderer::GraphInputs), an explicit choice rather than a stale image.
        const bool                           bloomProduced = textures.Transients.Bloom.IsValid();
        System::TonemapRenderer::GraphInputs graphInputs{ .Bloom = bloomProduced ? textures.Transients.Bloom
                                                                                 : textures.System.Black,
                                                          .BloomProduced = bloomProduced };
        if ( !graphInputs.Bloom.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] the tonemap has no bloom input (System.Black is not in this graph); "
                       "the tonemap pass is not recorded this frame" );
            return;
        }
        const RDG::TextureRef output = textures.Import( tonemap->GetOutputImage(), "Tonemap" );
        graph.AddPass(
             "PostFX: Tonemap", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 for ( const RDG::TextureRef read : reads )
                     if ( read.IsValid() )
                         pass.Read( read, RDG::Access::SampledGraphics );
                 pass.Read( graphInputs.Bloom, RDG::Access::SampledGraphics, RDG::SubresourceRange::Mip( 0 ) );
                 // A fullscreen quad writes every pixel: the old contents are not loaded.
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [tonemap, graphInputs]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return tonemap->Record( context, graphInputs ); } );
    }

    void SceneRenderer::AddFrameFXAA( RDG::Builder& graph, FrameTextures& textures )
    {
        auto* fxaa = UNIQUE_GET_AS( System::FXAARenderer, m_RenderSystems["FXAASystem"] );
        if ( !fxaa )
            return;
        const RDG::TextureRef input  = textures.Import( fxaa->GetInputImage(), "Tonemap" );
        const RDG::TextureRef output = textures.Import( fxaa->GetOutputImage(), "FXAA" );
        graph.AddPass(
             "PostFX: FXAA", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( input, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [fxaa]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 fxaa->Record();
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameSMAA( RDG::Builder& graph, FrameTextures& textures )
    {
        auto* smaa = UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] );
        if ( !smaa )
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
                 pass.Read( input, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, edges, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [smaa, input]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return smaa->RecordEdges( context, input ); } );
        graph.AddPass(
             "PostFX: SMAAWeights", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( edges, RDG::Access::SampledGraphics );
                 pass.Read( area, RDG::Access::SampledGraphics );
                 pass.Read( search, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, weights, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [smaa, edges, area, search]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return smaa->RecordWeights( context, edges, area, search ); } );
        graph.AddPass(
             "PostFX: SMAABlend", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( input, RDG::Access::SampledGraphics );
                 pass.Read( weights, RDG::Access::SampledGraphics );
                 pass.Read( edges, RDG::Access::SampledGraphics );
                 pass.Read( area, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [smaa, input, weights, edges, area]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return smaa->RecordBlend( context, input, weights, edges, area ); } );
    }

    RDG::TextureRef SceneRenderer::AddFrameBackdropBlur( RDG::Builder& graph, FrameTextures& textures,
                                                         const std::vector<RDG::TextureRef>& sceneColor )
    {
        auto* backdrop = UNIQUE_GET_AS( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] );
        if ( !backdrop || !backdrop->Prepare() )
            return {};
        const RDG::TextureRef pyramid = textures.Import( backdrop->GetImage(), "BackdropBlur" );
        const uint32_t        mips    = backdrop->GetMipLevels();

        // Scene -> pyramid mip 0, then mip i-1 -> mip i: one node per dispatch, declaring the one mip it samples
        // and the one it writes.
        for ( uint32_t mip = 0; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "UI: BackdropBlur{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     if ( mip == 0 )
                         ReadEach( pass, sceneColor, RDG::Access::SampledCompute );
                     else
                         pass.Read( pyramid, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ) );
                     pass.Write( pyramid, RDG::Access::StorageWrite, RDG::SubresourceRange::Mip( mip ) );
                 },
                 [backdrop, mip]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     backdrop->RecordDownsample( mip );
                     return BOOLSUCCESS;
                 } );
        // The UI phase samples the pyramid: the caller declares that read on the UI passes.
        return pyramid;
    }
} // namespace Desert::Graphic

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

    void SceneRenderer::AddFrameJumpFlood( RDG::Builder& graph, LegacyFrameTextures& textures )
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
        const RDG::TextureRef seeds[2] = { textures.Import( jfa->GetSeedImage( 0 ), "JumpFlood.Seed0" ),
                                           textures.Import( jfa->GetSeedImage( 1 ), "JumpFlood.Seed1" ) };
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
                 [jfa]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     jfa->RecordInit();
                     return BOOLSUCCESS;
                 } );
        }
        // Ping-pong propagation: one node per step, each sampling the seed the previous one wrote.
        if ( jfa->RunsSteps() )
            for ( uint32_t step = 0; step < jfa->GetStepCount(); ++step )
            {
                const uint32_t source = System::JumpFloodOutlineRenderer::GetStepSource( step );
                graph.AddPass(
                     std::format( "PostFX: JumpFloodStep{}", step ), RDG::PassFlags::Raster,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Read( seeds[source], RDG::Access::SampledGraphics );
                         pass.ColorTarget( 0, seeds[1 - source], clear );
                     },
                     [jfa, step]( RDG::PassContext& ) -> Common::BoolResultStr
                     {
                         jfa->RecordStep( step );
                         return BOOLSUCCESS;
                     } );
            }
        // The composite runs on every frame, outlined or not: it is what hands the scene colour to the tonemap
        // (TonemapRenderer::Inputs::Source is GetOutputImage()).
        const RDG::TextureRef scene  = textures.Import( jfa->GetSceneColorImage(), "JumpFlood.Scene" );
        const RDG::TextureRef output = textures.Import( jfa->GetOutputImage(), "Tonemap.Source" );
        const uint32_t        seed   = jfa->GetFinalSeedIndex();
        graph.AddPass(
             "PostFX: JumpFloodFinal", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( scene, RDG::Access::SampledGraphics );
                 pass.Read( seeds[seed], RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, output, clear );
             },
             [jfa]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 jfa->RecordFinal();
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameAutoExposure( RDG::Builder& graph, LegacyFrameTextures& textures,
                                              const std::vector<RDG::TextureRef>& sceneColor )
    {
        auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( !autoExp || !autoExp->Prepare() )
            return;
        // The tonemap samples the luminance this frame writes; that image is known when the graph is built.
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetAutoExposureImage( autoExp->GetAdaptedLuminanceImage() );
        const RDG::TextureRef previous =
             textures.Import( autoExp->GetPreviousLuminanceImage(), "AutoExposure.Previous" );
        const RDG::TextureRef adapted =
             textures.Import( autoExp->GetAdaptedLuminanceImage(), "AutoExposure.Adapted" );

        // The histogram is a storage buffer the graph does not track (no buffer import yet): Clear and Histogram
        // write only it, so they are culling roots.
        graph.AddPass(
             "PostFX: AutoExposureClear", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             []( RDG::PassBuilder& ) {},
             [autoExp]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 autoExp->RecordClear();
                 return BOOLSUCCESS;
             } );
        graph.AddPass(
             "PostFX: AutoExposureHistogram", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& pass ) { ReadEach( pass, sceneColor, RDG::Access::SampledCompute ); },
             [autoExp]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 autoExp->RecordHistogram();
                 return BOOLSUCCESS;
             } );
        graph.AddPass(
             "PostFX: AutoExposureAverage", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( previous, RDG::Access::SampledCompute );
                 pass.Write( adapted, RDG::Access::StorageWrite );
             },
             [autoExp]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 autoExp->RecordAverage();
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameBloom( RDG::Builder& graph, LegacyFrameTextures& textures,
                                       const std::vector<RDG::TextureRef>& sceneColor )
    {
        auto* bloom = UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] );
        if ( !bloom || !bloom->Prepare() )
            return;
        const RDG::TextureRef chain = textures.Import( bloom->GetBloomImage(), "Bloom" );
        const uint32_t        mips  = bloom->GetMipLevels();

        // Downsample: scene -> mip 0 (Karis + threshold), then mip i-1 -> mip i. One node per dispatch, each
        // declaring the one mip it samples and the one it writes.
        for ( uint32_t mip = 0; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "PostFX: BloomDownsample{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     if ( mip == 0 )
                         ReadEach( pass, sceneColor, RDG::Access::SampledCompute );
                     else
                         pass.Read( chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ) );
                     pass.Write( chain, RDG::Access::StorageWrite, RDG::SubresourceRange::Mip( mip ) );
                 },
                 [bloom, mip]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     bloom->RecordDownsample( mip );
                     return BOOLSUCCESS;
                 } );
        // Upsample (additive): mip i -> mip i-1, walking back to mip 0 (read-modify-write of the target mip).
        for ( uint32_t mip = mips - 1; mip >= 1; --mip )
            graph.AddPass(
                 std::format( "PostFX: BloomUpsample{}", mip ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Read( chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip ) );
                     pass.Write( chain, RDG::Access::StorageWrite, RDG::SubresourceRange::Mip( mip - 1 ) );
                 },
                 [bloom, mip]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     bloom->RecordUpsample( mip );
                     return BOOLSUCCESS;
                 } );
    }

    void SceneRenderer::AddFrameLightShafts( RDG::Builder& graph, LegacyFrameTextures& textures,
                                             const std::vector<RDG::TextureRef>&       sceneColor,
                                             const std::shared_ptr<LegacyFrameValues>& values )
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

    void SceneRenderer::AddFrameLensFlare( RDG::Builder& graph, LegacyFrameTextures& textures,
                                           const std::vector<RDG::TextureRef>&       sceneColor,
                                           const std::shared_ptr<LegacyFrameValues>& values )
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

    void SceneRenderer::AddFrameTonemap( RDG::Builder& graph, LegacyFrameTextures& textures )
    {
        auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( !tonemap )
            return;
        const System::TonemapRenderer::Inputs inputs = tonemap->GetInputs();
        const std::vector<RDG::TextureRef>    reads  = {
             textures.Import( inputs.Source, "Tonemap.Source" ),
             textures.Import( inputs.Bloom, "Bloom" ),
             textures.Import( inputs.AutoExposure, "AutoExposure.Adapted" ),
             textures.Import( inputs.LightShafts, "LightShaft.Pong" ),
             textures.Import( inputs.LensFlare, "LensFlare" ),
        };
        const RDG::TextureRef output = textures.Import( tonemap->GetOutputImage(), "Tonemap" );
        graph.AddPass(
             "PostFX: Tonemap", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 for ( const RDG::TextureRef read : reads )
                     if ( read.IsValid() )
                         pass.Read( read, RDG::Access::SampledGraphics );
                 // A fullscreen quad writes every pixel: the old contents are not loaded.
                 pass.ColorTarget( 0, output, RDG::LoadOp::DontCare() );
             },
             [tonemap]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 tonemap->Record();
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameFXAA( RDG::Builder& graph, LegacyFrameTextures& textures )
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

    void SceneRenderer::AddFrameSMAA( RDG::Builder& graph, LegacyFrameTextures& textures )
    {
        auto* smaa = UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] );
        if ( !smaa || !smaa->Prepare() )
            return;
        const RDG::TextureRef input   = textures.Import( smaa->GetInputImage(), "Tonemap" );
        const RDG::TextureRef edges   = textures.Import( smaa->GetEdgesImage(), "SMAA.Edges" );
        const RDG::TextureRef weights = textures.Import( smaa->GetWeightsImage(), "SMAA.Weights" );
        const RDG::TextureRef area    = textures.Import( smaa->GetAreaTex(), "SMAA.AreaTex" );
        const RDG::TextureRef search  = textures.Import( smaa->GetSearchTex(), "SMAA.SearchTex" );
        const RDG::TextureRef output  = textures.Import( smaa->GetOutputImage(), "SMAA" );
        // The old passes cleared edges and weights (RenderPassSpecification's default clear, black); the edge
        // shader discards pixels without an edge, so the clear is kept.
        graph.AddPass(
             "PostFX: SMAAEdges", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( input, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, edges, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [smaa]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 smaa->RecordEdges();
                 return BOOLSUCCESS;
             } );
        graph.AddPass(
             "PostFX: SMAAWeights", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( edges, RDG::Access::SampledGraphics );
                 pass.Read( area, RDG::Access::SampledGraphics );
                 pass.Read( search, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, weights, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [smaa]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 smaa->RecordWeights();
                 return BOOLSUCCESS;
             } );
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
             [smaa]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 smaa->RecordBlend();
                 return BOOLSUCCESS;
             } );
    }

    RDG::TextureRef SceneRenderer::AddFrameBackdropBlur( RDG::Builder& graph, LegacyFrameTextures& textures,
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

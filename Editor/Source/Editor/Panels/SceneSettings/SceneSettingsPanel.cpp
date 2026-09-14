#include "SceneSettingsPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Image.hpp>

#include <algorithm>
#include <format>

#include <ImGui/imgui.h>
#include <glm/gtc/type_ptr.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    SceneSettingsPanel::SceneSettingsPanel( std::shared_ptr<::Desert::Core::Scene> scene )
         : IPanel( "Scene Settings" ), m_Scene( std::move( scene ) ),
           m_UIHelper( std::make_unique<UI::UIHelper>() )
    {
        m_UIHelper->Init();
    }

    void SceneSettingsPanel::OnUIRender()
    {
        if ( !m_Scene )
        {
            ImGui::TextDisabled( "No active scene" );
            return;
        }

        Core::SceneSettings& s = m_Scene->GetSettings();

        if ( Utils::ImGuiUtilities::SectionHeader( "Anti-Aliasing" ) )
        {
            // TWO CONTROLS OVER ONE OWNER, AND THAT IS WHAT К3 CHANGED HERE.
            //
            // This started as ONE combo with four entries — None / FXAA / SMAA / MSAA — over two pieces of
            // state with different owners: SceneSettings::AA travelled with the level, MSAASamples was
            // this machine's. Picking MSAA wrote BOTH (`s.AA = None` and `prefs.MSAASamples = 4`), which
            // §4.2 forbids: one user action, one value, two places to store it. К2 split it into two
            // controls labelled "(scene)" and "(this machine)", which stopped the cross-write and left the
            // real problem standing — half of one question stored in a file that travels to everybody, so
            // a machine that could not afford SMAA had to commit a change to fix it.
            //
            // Both halves are the machine's now, in one file that BOTH the editor and the packaged game
            // read. They are still two controls because they are still two things: MSAA resolves geometry
            // edges inside the pipeline and post AA filters the resolved image, so both can be on.
            auto&     quality = Common::Settings::MachineSettings::Get();
            const int maxMsaa = Graphic::RenderConfig::MaxMSAASamples.load();

            const char* modes[] = { "None", "FXAA", "SMAA" };
            int         current = static_cast<int>( quality.AA );
            if ( ImGui::Combo( "Post AA (this machine)", &current, modes, IM_ARRAYSIZE( modes ) ) )
            {
                quality.AA = static_cast<Common::Settings::AntiAliasingMode>( current );
                Common::Settings::MachineSettings::Save();
            }
            Utils::ImGuiUtilities::Tooltip(
                 "A post-process pass on the finished image; applies immediately. Belongs to this "
                 "machine, not to the scene — TAA/DLSS need motion vectors (deferred)." );

            const char* msaaLevels[] = { "Off", "2x", "4x", "8x" };
            const int   msaaValues[] = { 1, 2, 4, 8 };
            int         msaaIdx      = 0;
            for ( int v = 0; v < IM_ARRAYSIZE( msaaValues ); ++v )
                if ( msaaValues[v] == quality.MSAASamples )
                    msaaIdx = v;
            if ( ImGui::Combo( "MSAA (this machine)", &msaaIdx, msaaLevels, IM_ARRAYSIZE( msaaLevels ) ) )
            {
                quality.MSAASamples = std::min( msaaValues[msaaIdx], maxMsaa );
                Common::Settings::MachineSettings::Save();
            }
            Utils::ImGuiUtilities::Tooltip(
                 "Hardware multisampling. The pipelines bake their sample count at startup, so it costs a "
                 "restart and belongs to the machine that pays for it." );
            ImGui::TextDisabled( "Device max: %dx. Both may be on — MSAA resolves edges, post AA filters\n"
                                 "the resolved image.",
                                 maxMsaa );

            // MSAA bakes into the pipelines at startup — flag any pending change loudly.
            const int active = Graphic::RenderConfig::MSAASamplesActive.load();
            if ( quality.MSAASamples != active )
                ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.2f, 1.0f ),
                                    "Restart the editor to apply MSAA (now: %s, selected: %s).",
                                    active > 1 ? std::format( "{}x", active ).c_str() : "off",
                                    quality.MSAASamples > 1 ? std::format( "{}x", quality.MSAASamples ).c_str()
                                                            : "off" );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Post-Processing" ) )
        {
            // The operator first: it decides what the knobs below it mean, and one of them (White Point)
            // exists only for Reinhard.
            const char* tonemappers[] = { "ACES (filmic)", "Reinhard (extended)" };
            int         tonemapper    = static_cast<int>( s.Tonemapper );
            if ( ImGui::Combo( "Tonemapper (scene)", &tonemapper, tonemappers, IM_ARRAYSIZE( tonemappers ) ) )
                s.Tonemapper = static_cast<Core::TonemapOperator>( tonemapper );
            Utils::ImGuiUtilities::Tooltip(
                 "The curve that maps HDR luminance onto the display. ACES is the default and is the "
                 "film curve Unreal grades through, so frames of ours can be measured against frames of "
                 "theirs. Reinhard is the operator every scene authored before 2026-08-19 was lit on; it "
                 "takes a White Point, ACES does not." );

            // Live — consumed by the tonemap pass.
            ImGui::Checkbox( "Auto Exposure (eye adaptation)", &s.AutoExposure );
            ImGui::BeginDisabled( !s.AutoExposure );
            ImGui::SliderFloat( "Exposure Key", &s.AutoExposureKey, 0.01f, 1.0f );
            ImGui::SliderFloat( "Adapt Speed", &s.AutoExposureSpeed, 0.1f, 10.0f );
            ImGui::SliderFloat( "Min Luma", &s.AutoExposureMin, 0.001f, 1.0f, "%.3f" );
            ImGui::SliderFloat( "Max Luma", &s.AutoExposureMax, 1.0f, 32.0f );
            ImGui::EndDisabled();

            ImGui::BeginDisabled( s.AutoExposure );
            ImGui::SliderFloat( "Exposure (manual)", &s.Exposure, 0.0f, 5.0f );
            ImGui::EndDisabled();
            ImGui::SliderFloat( "Gamma", &s.Gamma, 1.0f, 3.0f );

            // White Point is extended Reinhard's parameter and nothing else's — the ACES fit carries the
            // white of the ODT it reproduces. Shown only in the mode that reads it, the same way
            // Anisotropy below appears only in Anisotropic filtering: a slider that moves nothing in the
            // current mode is a dead setting, and dead settings are how "half the options do nothing"
            // starts.
            if ( s.Tonemapper == Core::TonemapOperator::Reinhard )
            {
                ImGui::SliderFloat( "White Point", &s.WhitePoint, 1.0f, 20.0f );
                Utils::ImGuiUtilities::Tooltip(
                     "Luminance that maps to pure white. At 1.0 the operator is the identity and every "
                     "highlight above 1.0 clips to flat white; raise it to keep shading inside bright "
                     "content (the sun's surroundings, emissive surfaces)." );
            }

            ImGui::Spacing();
            ImGui::Checkbox( "Bloom", &s.EnableBloom );
            ImGui::SliderFloat( "Bloom Threshold", &s.BloomThreshold, 0.0f, 5.0f );
            ImGui::SliderFloat( "Bloom Intensity", &s.BloomIntensity, 0.0f, 3.0f );
            ImGui::SliderFloat( "Lens Dispersion", &s.LensDispersion, 0.0f, 3.0f );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Chromatic rainbow fringe around bright sources (glare). Needs Bloom on." );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Lens Flare" ) )
        {
            ImGui::Checkbox( "Lens Flare", &s.EnableLensFlare );
            Utils::ImGuiUtilities::Tooltip(
                 "Ghosts, halo and anamorphic streak from the sun disc. Every feature is an image of "
                 "what is actually bright on screen, so an occluded sun dims its own flare." );

            ImGui::BeginDisabled( !s.EnableLensFlare );
            ImGui::SliderFloat( "Flare Intensity", &s.LensFlareIntensity, 0.0f, 5.0f );
            ImGui::ColorEdit3( "Flare Tint", &s.LensFlareTint.x );
            ImGui::SliderFloat( "Flare Threshold", &s.LensFlareThreshold, 0.0f, 50.0f );
            Utils::ImGuiUtilities::Tooltip(
                 "HDR luminance a pixel must exceed to flare. Keep it above the brightest sky so only "
                 "the sun qualifies." );

            ImGui::Spacing();
            ImGui::SliderInt( "Ghost Count", &s.LensFlareGhostCount, 0, 8 );
            ImGui::SliderFloat( "Ghost Spacing", &s.LensFlareGhostSpacing, 0.05f, 1.5f );
            Utils::ImGuiUtilities::Tooltip(
                 "Fraction of the sun-to-centre axis between one ghost and the next. Past 1.0 of the way "
                 "the train continues on the far side of the screen centre." );
            ImGui::SliderFloat( "Ghost Size Near", &s.LensFlareGhostSizeNear, 0.05f, 16.0f );
            Utils::ImGuiUtilities::Tooltip(
                 "Magnification of the source, not a screen size: 1.0 draws a ghost the size the sun "
                 "already is." );
            ImGui::SliderFloat( "Ghost Size Far", &s.LensFlareGhostSizeFar, 0.05f, 16.0f );
            ImGui::ColorEdit3( "Ghost Tint Inner", &s.LensFlareGhostTintInner.x );
            ImGui::ColorEdit3( "Ghost Tint Outer", &s.LensFlareGhostTintOuter.x );

            ImGui::Spacing();
            ImGui::SliderFloat( "Halo Intensity", &s.LensFlareHaloIntensity, 0.0f, 3.0f );
            ImGui::SliderFloat( "Halo Radius", &s.LensFlareHaloRadius, 0.02f, 1.0f );

            ImGui::Spacing();
            ImGui::SliderFloat( "Streak Intensity", &s.LensFlareStreakIntensity, 0.0f, 30.0f );
            Utils::ImGuiUtilities::Tooltip(
                 "Ranges an order above the other intensities because the streak is an average over its "
                 "taps, so it meets the authored number at a different scale than a ghost does." );
            ImGui::SliderFloat( "Streak Length", &s.LensFlareStreakLength, 0.0f, 1.0f );
            ImGui::SliderFloat( "Streak Angle", &s.LensFlareStreakAngle, -180.0f, 180.0f );

            ImGui::Spacing();
            ImGui::SliderFloat( "Chromatic Shift", &s.LensFlareChromaShift, 0.0f, 1.0f );
            Utils::ImGuiUtilities::Tooltip(
                 "How far apart the three channels are read within each feature. The fringe is the scene "
                 "dispersed, not a colour painted on — 0 gives a neutral flare." );
            ImGui::EndDisabled();
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Textures" ) )
        {
            auto& quality = Common::Settings::MachineSettings::Get();

            // Global sampler filter — applies live (samplers are recreated when this changes).
            const char* items[]  = { "Nearest", "Bilinear", "Trilinear", "Anisotropic" };
            int         current  = static_cast<int>( quality.TextureFilterMode );
            if ( ImGui::Combo( "Filter (this machine)", &current, items, IM_ARRAYSIZE( items ) ) )
            {
                quality.TextureFilterMode = static_cast<Common::Settings::TextureFilter>( current );
                Common::Settings::MachineSettings::Save();
            }
            Utils::ImGuiUtilities::Tooltip( "Sampler filter for every texture. The same picture, sharper "
                                            "or blurrier — this machine's choice, not the level's." );

            // Anisotropy level — only meaningful in Anisotropic mode. Shown only there for the reason
            // White Point above is shown only under Reinhard: a control that moves nothing in the current
            // mode is a dead setting.
            if ( quality.TextureFilterMode == Common::Settings::TextureFilter::Anisotropic )
            {
                const char* levels[]  = { "1x", "2x", "4x", "8x", "16x" };
                const int   values[]  = { 1, 2, 4, 8, 16 };
                int         levelIdx  = 3; // default 8x
                for ( int i = 0; i < IM_ARRAYSIZE( values ); ++i )
                    if ( values[i] == quality.Anisotropy )
                        levelIdx = i;
                if ( ImGui::Combo( "Anisotropy (this machine)", &levelIdx, levels, IM_ARRAYSIZE( levels ) ) )
                {
                    quality.Anisotropy = values[levelIdx];
                    Common::Settings::MachineSettings::Save();
                }
            }
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Environment", false ) )
        {
            // Live — cascaded directional shadow maps.
            ImGui::Checkbox( "Shadows", &s.EnableShadows );
            ImGui::SliderFloat( "Shadow Bias", &s.ShadowBias, 0.0f, 0.02f, "%.4f" );
            ImGui::SliderFloat( "Cascade Split Lambda", &s.CascadeSplitLambda, 0.0f, 1.0f );

            ImGui::Spacing();
            ImGui::TextDisabled( "Procedural Sky + skybox intensity moved to the Skybox component (Details)." );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Shadow Maps", false ) )
        {
            // WHAT IS LEFT HERE IS AN INSPECTOR, NOT A SETTING, and that is the whole distinction К2 drew
            // through this panel. The "Shadow Debug" combo that used to sit above these thumbnails wrote
            // SceneSettings::ShadowDebug — a viewport visualization stored in the level file — and it
            // duplicated the viewport's own View Mode dropdown, which offered Cascades but not Shadow
            // Factor. Both modes live in that dropdown now (View Mode -> Shadow Cascades / Shadow Factor)
            // and the flag lives in EditorPreferences. The images below read GPU state and write nothing,
            // so they stay.

            // CSM cascade depth maps (R32F light-space depth, near→far cascades).
            if ( auto* sr = m_Scene->GetSceneRenderer() )
            {
                const uint32_t count = sr->GetShadowCascadeCount();
                ImGui::TextDisabled( "CSM cascade depth maps (near -> far):" );
                constexpr float kThumb = 96.0f;
                for ( uint32_t c = 0; c < count; ++c )
                {
                    if ( auto img = sr->GetShadowCascadeImage( c ) )
                        m_UIHelper->Image( img, ImVec2( kThumb, kThumb ) );
                    if ( ( c % 3 ) != 2 && c + 1 < count )
                        ImGui::SameLine();
                }
            }
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Rendering" ) )
        {
            // EVERY CONTROL IN THIS SECTION SAYS WHO OWNS IT, and the four that say "(scene)" are the
            // four that LOOK like machine quality and are not. К1's test decides it: set to its cheapest
            // value, has the level been mis-authored or merely rendered worse? Forward and Deferred
            // disagree about cloud shadow on the ground, GI Off is a darker room rather than a coarser
            // one, and SSAO/SSR are on-offs rather than fidelity ladders — all four change the authored
            // look. Cloud Quality below does not, so it moved with its four siblings (К3).
            const char* paths[] = { "Forward", "Deferred" };
            int         cur     = static_cast<int>( s.RenderingPath );
            if ( ImGui::Combo( "Render Path (scene)", &cur, paths, IM_ARRAYSIZE( paths ) ) )
                s.RenderingPath = static_cast<Core::RenderPath>( cur );

            // The volumetric cloud layer's cost ceiling. OUTSIDE the deferred-only block below, because
            // the clouds are marched and composited on both paths — only the shadow they cast on the
            // world is deferred-only, and that is a part of what this tier scales rather than the whole.
            auto&       quality        = Common::Settings::MachineSettings::Get();
            const char* cloudQuality[] = { "Low", "Medium", "High" };
            int         cloudCur       = static_cast<int>( quality.CloudQualityTier );
            if ( ImGui::Combo( "Cloud Quality (this machine)", &cloudCur, cloudQuality,
                               IM_ARRAYSIZE( cloudQuality ) ) )
            {
                quality.CloudQualityTier = static_cast<Common::Settings::CloudQuality>( cloudCur );
                Common::Settings::MachineSettings::Save();
            }
            ImGui::TextDisabled( "High is the calibrated reference. Medium halves the cloud shadow map's\n"
                                 "reach on the ground (~15 km); Low also caps the sun-ray at 16 samples,\n"
                                 "which runs the sunward highlights bright." );

            // The "Deferred Debug" combo that used to sit here is gone (К2): a G-buffer view is what the
            // VIEWPORT is showing, not what the level is, and this combo was a second control over the
            // same state as the viewport's View Mode dropdown — offering GI, which that one lacked, and
            // lacking the three heat maps, which it had. GI was added there; this is the one control now.
            ImGui::BeginDisabled( s.RenderingPath != Core::RenderPath::Deferred );
            ImGui::Checkbox( "Enable SSAO (scene)", &s.EnableSSAO );

            const char* giModes[] = { "Off", "Screen Space", "RSM" };
            int         giCur     = static_cast<int>( s.GlobalIllumination );
            if ( ImGui::Combo( "Global Illumination (scene)", &giCur, giModes, IM_ARRAYSIZE( giModes ) ) )
                s.GlobalIllumination = static_cast<Core::GIMode>( giCur );
            ImGui::BeginDisabled( s.GlobalIllumination == Core::GIMode::Off );
            ImGui::SliderFloat( "GI Intensity", &s.GIIntensity, 0.0f, 20.0f );
            ImGui::EndDisabled();
            if ( s.GlobalIllumination == Core::GIMode::RSM )
                ImGui::TextDisabled( "RSM bounces off-screen geometry; its gather is dim (try ~6)." );

            ImGui::Checkbox( "Enable SSR (scene)", &s.EnableSSR );
            ImGui::BeginDisabled( !s.EnableSSR );
            ImGui::SliderFloat( "SSR Intensity", &s.SSRIntensity, 0.0f, 1.0f );
            // World units are centimetres; the bounds mirror the PROPERTY Range on SSRMaxDistance
            // (1 m .. 200 m). The old 1..200 pair was the metre-era slider consumed as centimetres, which
            // capped the reflection ray at two metres.
            ImGui::SliderFloat( "SSR Max Distance", &s.SSRMaxDistance, 100.0f, 20000.0f, "%.0f cm" );
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::TextDisabled( "Deferred: static meshes only, directional light (WIP)." );
        }

        // Shared wind — a scene-global environment force (not owned by the Skybox). It reaches
        // SceneRenderer::GetWind() and stops there: Г25 removed the procedural grass, which was its only
        // reader, and the next one is whatever sways an ASSET (instanced foliage, hair, cloth). The three
        // values are authored level data that every scene in the repository states, so they were raised
        // with the owner rather than retired by a rendering task - see SceneRenderer::GetWind().
        if ( Utils::ImGuiUtilities::SectionHeader( "Wind" ) )
        {
            ImGui::SliderFloat( "Direction (deg)", &s.WindDirection, 0.0f, 360.0f );
            ImGui::SliderFloat( "Strength", &s.WindStrength, 0.0f, 1.0f );
            ImGui::SliderFloat( "Turbulence", &s.WindTurbulence, 0.0f, 3.0f );
            ImGui::TextDisabled( "Shared: one direction moves every wind-driven renderer." );
        }

        // NOTE: the Time of Day section was removed on purpose — the sun's state is the directional
        // light entity itself (its position IS the direction; the sky/lighting follow it). One
        // source of truth, no second global knob fighting the hand-placed sun.
    }
} // namespace Desert::Editor

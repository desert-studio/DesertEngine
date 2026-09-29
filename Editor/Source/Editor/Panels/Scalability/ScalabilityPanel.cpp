#include "ScalabilityPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <Engine/Graphic/RenderConfig.hpp>

#include <algorithm>
#include <format>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    // UE's Scalability / GameUserSettings: what THIS MACHINE can afford. Every control here writes
    // Common::Settings::MachineSettings (machine.json) and nothing reaches a scene file. They lived in the
    // Scene Settings panel, marked "(this machine)", until SET1 gave them their own window.
    // Starts CLOSED, like the other tools in Window -> Tools (Localization): it is opened on purpose, and a
    // default-visible floating window sat on top of the viewport at every start on a fresh profile.
    ScalabilityPanel::ScalabilityPanel() : IPanel( "Scalability", /*showPanel=*/false )
    {
    }

    void ScalabilityPanel::OnUIRender()
    {
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

        if ( Utils::ImGuiUtilities::SectionHeader( "Textures" ) )
        {
            auto& quality = Common::Settings::MachineSettings::Get();

            // Global sampler filter — applies live (samplers are recreated when this changes).
            const char* items[] = { "Nearest", "Bilinear", "Trilinear", "Anisotropic" };
            int         current = static_cast<int>( quality.TextureFilterMode );
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
                const char* levels[] = { "1x", "2x", "4x", "8x", "16x" };
                const int   values[] = { 1, 2, 4, 8, 16 };
                int         levelIdx = 3; // default 8x
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

        if ( Utils::ImGuiUtilities::SectionHeader( "Clouds" ) )
        {
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
        }
    }
} // namespace Desert::Editor

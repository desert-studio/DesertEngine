#include "ScalabilityPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneSettings.hpp>
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
            // ONE METHOD (AA1, UE's r.AntiAliasingMethod + r.MSAACount): None / FXAA / SMAA / MSAA are
            // alternatives, and the sample count is a second control shown only under MSAA — a count that
            // moves nothing in the current method would be a dead setting. Every change applies on the next
            // frame: SceneRenderer recreates its target at the new count, so there is no restart note.
            //
            // MSAA ONLY WHERE IT WORKS (AA2, as UE): a deferred scene lists None / FXAA / SMAA, and a stored
            // MSAA choice shows as what the frame runs there (FXAA, MachineSettings::EffectiveAA) with a line
            // saying why. The stored choice is not rewritten by looking: it applies again in a forward scene.
            auto&     quality = Common::Settings::MachineSettings::Get();
            const int maxMsaa = Graphic::RenderConfig::MaxMSAASamples.load();

            const auto scene = m_Scene.lock();
            const bool forwardScene =
                 !scene || Desert::Core::RenderPathSupportsMSAA( scene->GetSettings().RenderingPath );
            const Common::Settings::EffectiveAntiAliasing effective = quality.EffectiveAA( forwardScene );

            const char* methods[] = { "None", "FXAA", "SMAA", "MSAA" };
            const int   offered   = forwardScene ? IM_ARRAYSIZE( methods ) : IM_ARRAYSIZE( methods ) - 1;
            int         current =
                 static_cast<int>( effective.MSAAUnavailableOnPath ? effective.Method : quality.AAMethod );
            if ( ImGui::Combo( "Anti-Aliasing Method", &current, methods, offered ) )
            {
                quality.AAMethod = static_cast<Common::Settings::AntiAliasingMethod>( current );
                Common::Settings::MachineSettings::Save();
            }
            Utils::ImGuiUtilities::Tooltip(
                 forwardScene ? "FXAA and SMAA filter the finished image; MSAA renders the scene "
                                "at several samples per pixel. Applies on the next frame."
                              : "FXAA and SMAA filter the finished image. MSAA is offered in "
                                "forward scenes only: deferred lighting shades one sample per "
                                "pixel, so MSAA would not smooth solid objects here." );
            if ( effective.MSAAUnavailableOnPath )
                ImGui::TextDisabled( "%s",
                                     std::format( "MSAA {}x applies to forward scenes; this scene is deferred: "
                                                  "using FXAA.",
                                                  quality.MSAASamples )
                                          .c_str() );

            if ( forwardScene && quality.AAMethod == Common::Settings::AntiAliasingMethod::MSAA )
            {
                const char* levels[] = { "2x", "4x", "8x" };
                const int   values[] = { 2, 4, 8 };
                int         count    = 0; // the entries this device can run
                int         selected = 0;
                for ( int i = 0; i < IM_ARRAYSIZE( values ) && values[i] <= maxMsaa; ++i, ++count )
                    if ( values[i] == quality.MSAASamples )
                        selected = i;
                if ( count == 0 )
                    ImGui::TextDisabled( "This device has no multisampling (max %dx).", maxMsaa );
                else if ( ImGui::Combo( "Samples", &selected, levels, count ) )
                {
                    quality.MSAASamples = values[selected];
                    Common::Settings::MachineSettings::Save();
                }
            }
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

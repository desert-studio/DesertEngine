#include "WorldSettingsPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Image.hpp>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    // UE's World Settings: what the LEVEL is, and nothing else (SET1). The grade moved to PostProcessVolume
    // entities (Details), the shadow policy to the DirectionalLight (Details), and the machine-quality
    // controls to the Scalability window. Gravity and the splash are edited here; the render path stays a
    // level field because the corpus renders both paths side by side (the NRM_Witness / MAT_Probe pairs).
    WorldSettingsPanel::WorldSettingsPanel( std::shared_ptr<::Desert::Core::Scene> scene )
         : IPanel( "World Settings" ), m_Scene( std::move( scene ) ),
           m_UIHelper( std::make_unique<UI::UIHelper>() )
    {
        m_UIHelper->Init();
    }

    void WorldSettingsPanel::OnUIRender()
    {
        if ( !m_Scene )
        {
            ImGui::TextDisabled( "No active scene" );
            return;
        }

        Core::SceneSettings& s = m_Scene->GetSettings();

        if ( Utils::ImGuiUtilities::SectionHeader( "Rendering" ) )
        {
            // Forward and Deferred disagree about cloud shadow on the ground, so the path changes the
            // authored look (К1) and stays level data. UE puts it in Project Settings; here the corpus
            // renders both paths side by side (NRM_Witness / MAT_Probe pairs), which one project value
            // could not express.
            const char* paths[] = { "Forward", "Deferred" };
            int         cur     = static_cast<int>( s.RenderingPath );
            if ( ImGui::Combo( "Render Path (scene)", &cur, paths, IM_ARRAYSIZE( paths ) ) )
                s.RenderingPath = static_cast<Core::RenderPath>( cur );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Physics" ) )
            ImGui::SliderFloat( "Gravity", &s.Gravity, 0.0f, 5000.0f, "%.0f cm/s^2" );

        if ( Utils::ImGuiUtilities::SectionHeader( "Splash" ) )
        {
            ImGui::SliderFloat( "Splash Duration", &s.SplashDuration, 0.0f, 10.0f, "%.1f s" );
            ImGui::SliderFloat( "Splash Fade", &s.SplashFade, 0.0f, 3.0f, "%.2f s" );
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
    }
} // namespace Desert::Editor

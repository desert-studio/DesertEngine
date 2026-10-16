#include "WorldSettingsPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>

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
    // controls to the Scalability window.
    WorldSettingsPanel::WorldSettingsPanel( std::shared_ptr<::Desert::Core::Scene>          scene,
                                            std::shared_ptr<::Desert::Assets::AssetManager> assetManager )
         : IPanel( "World Settings" ), m_Scene( std::move( scene ) ), m_AssetManager( std::move( assetManager ) ),
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

        // One reflected pass, the way UE's World Settings is a DetailsView on AWorldSettings: categories,
        // ranges, enum combos, asset pickers, per-field reset and undo all come from the PROPERTY metadata.
        PropertyEditorBuilder::Draw( &m_Scene->GetSettings(), "SceneSettings", m_AssetManager.get(),
                                     m_UIHelper.get() );

        if ( Utils::ImGuiUtilities::SectionHeader( "Shadow Maps", false ) )
        {
            // WHAT IS LEFT HERE IS AN INSPECTOR, NOT A SETTING, and that is the whole distinction К2 drew
            // through this panel. The "Shadow Debug" combo that used to sit above these thumbnails wrote
            // SceneSettings::ShadowDebug — a viewport visualization stored in the level file — and it
            // duplicated the viewport's own View Mode dropdown, which offered Cascades but not Shadow
            // Factor. Both modes live in that dropdown now (View Mode -> Shadow Cascades / Shadow Factor)
            // and the flag lives in EditorPreferences. The images below read GPU state and write nothing,
            // so they stay.

            // CSM cascades' static-caster caches (D32F light-space depth, near→far cascades).
            if ( auto* sr = m_Scene->GetSceneRenderer() )
            {
                const uint32_t count = sr->GetShadowCascadeCount();
                ImGui::TextDisabled( "CSM static-caster caches (near -> far):" );
                constexpr float kThumb = 96.0f;
                for ( uint32_t c = 0; c < count; ++c )
                {
                    if ( auto img = sr->GetShadowCascadeCacheImage( c ) )
                        m_UIHelper->Image( img, ImVec2( kThumb, kThumb ) );
                    if ( ( c % 3 ) != 2 && c + 1 < count )
                        ImGui::SameLine();
                }
            }
        }
    }
} // namespace Desert::Editor

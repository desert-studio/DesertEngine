#include "PrefabComponentWidget.hpp"

#include <ImGui/imgui.h>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>

#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    PrefabComponentWidget::PrefabComponentWidget( const Assets::AssetManager* assetManager )
         : IComponentWidget( "Prefab" ), m_AssetManager( assetManager )
    {
    }

    void PrefabComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* /*scene*/ )
    {
        auto& prefab = entity.GetComponent<ECS::PrefabComponent>();

        Utils::ImGuiUtilities::PushID();
        Utils::ImGuiUtilities::ResetPropertyRows();

        // ── Prefab name / handle ──────────────────────────────────────────
        std::shared_ptr<Assets::PrefabAsset> asset;
        std::string                          displayName = "<None>";
        bool                                 valid       = false;

        if ( m_AssetManager && prefab.Prefab )
        {
            asset = m_AssetManager->FindByHandle<Assets::PrefabAsset>( prefab.Prefab );
            if ( asset )
            {
                displayName = asset->GetMetadata().Filepath.filename().string();
                valid       = true;
            }
            else
            {
                displayName = "Missing (" + std::to_string( prefab.Prefab ) + ")";
            }
        }

        Utils::ImGuiUtilities::BeginPropertyRow( "Asset" );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( displayName.c_str() );
        Utils::ImGuiUtilities::EndPropertyRow();

        Utils::ImGuiUtilities::BeginPropertyRow( "Status" );
        ImGui::AlignTextToFramePadding();
        if ( valid )
            ImGui::TextColored( ThemeManager::GetSuccessColor(), ICON_MDI_CHECK " Valid" );
        else if ( prefab.Prefab )
            ImGui::TextColored( ThemeManager::GetErrorColor(), ICON_MDI_ALERT " Missing" );
        else
            ImGui::TextColored( ImVec4( 0.6f, 0.6f, 0.6f, 1.f ), ICON_MDI_MINUS " None" );
        Utils::ImGuiUtilities::EndPropertyRow();

        // ── Select prefab by path ─────────────────────────────────────────
        ImGui::TextUnformatted( "Override Path" );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - 60.0f );
        Utils::ImGuiUtilities::InputText( m_SelectPathBuf, "##PrefabSelectPath" );
        ImGui::SameLine();
        if ( ImGui::Button( "Load" ) && !m_SelectPathBuf.empty() )
        {
            auto found = const_cast<Assets::AssetManager*>( m_AssetManager )
                ->CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High, m_SelectPathBuf );
            if ( found )
            {
                found->Load();
                prefab.Prefab = found->GetMetadata().Handle;
            }
        }

        ImGui::Separator();

        // ── Action buttons ────────────────────────────────────────────────
        if ( valid )
        {
            if ( ImGui::Button( ICON_MDI_REFRESH " Apply to Prefab" ) )
            {
                asset->CreateFromEntity( entity, *m_AssetManager );
                const auto        serializedText = asset->Serialize();
                const std::string serialized     = serializedText ? serializedText.GetValue() : std::string();
                // The directory is created for the same reason SceneCommands::ApplyPrefabInstance
                // creates it — an ofstream into a missing directory writes nothing at all — and this
                // twin of that function was the one place that did not.
                std::error_code dirEc;
                std::filesystem::create_directories( asset->GetMetadata().Filepath.parent_path(), dirEc );

                const auto written = ( serializedText ? Common::Utils::FileSystem::WriteContentToFileAtomic(
                                                             asset->GetMetadata().Filepath, serialized )
                                                      : Common::MakeError<bool>( serializedText.GetError() ) );
                if ( written )
                {
                    LOG_INFO( "Prefab applied: {0}", asset->GetMetadata().Filepath.string() );
                }
                else
                {
                    LOG_ERROR( "[Prefab] '{}' was NOT written: {} — the file on disk still holds the "
                               "previous state.",
                               asset->GetMetadata().Filepath.string(), written.GetError() );
                }
            }
            Utils::ImGuiUtilities::Tooltip( "Overwrite the prefab file with the current entity state" );

            ImGui::SameLine();

            if ( ImGui::Button( ICON_MDI_PACKAGE_VARIANT_CLOSED " Unpack" ) )
            {
                // Remove PrefabComponent from root — children keep their components
                entity.GetRegistry()->remove<ECS::PrefabComponent>( entity.GetHandle() );
                Utils::ImGuiUtilities::PopID();
                return; // component is gone; stop rendering this widget
            }
            Utils::ImGuiUtilities::Tooltip( "Break the prefab link; entity and children become regular objects" );

            ImGui::SameLine();
        }

        if ( ImGui::Button( ICON_MDI_CLOSE " Clear" ) )
            prefab.Prefab = Common::UUID::Null();

        // ── Debug ─────────────────────────────────────────────────────────
        if ( ImGui::TreeNodeEx( "Debug", ImGuiTreeNodeFlags_Framed ) )
        {
            ImGui::Text( "Handle: %llu", (unsigned long long)prefab.Prefab );
            if ( asset )
                ImGui::Text( "Path: %s", asset->GetMetadata().Filepath.string().c_str() );
            ImGui::TreePop();
        }

        Utils::ImGuiUtilities::PopID();
    }

    // The widget existed since the prefab work landed but was never registered, so a prefab instance had
    // NO Details section at all — apply/unpack/clear were unreachable and the only prefab affordance in
    // the panel was "Revert To Prefab", buried in the header's gear popup.
    DESERT_REGISTER_CUSTOM_COMPONENT( ECS::PrefabComponent, "Prefab", true,
                                      ( []( ECS::Entity& e, ::Desert::Core::Scene* s,
                                            const ComponentEditContext& ctx )
                                        { PrefabComponentWidget( ctx.AssetMgr() ).Render( e, s ); } ) )
} // namespace Desert::Editor

#include "PrefabComponentWidget.hpp"

#include <ImGui/imgui.h>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>

#include <Engine/Assets/Prefab/PackedPrefab.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Runtime/Factory/PrefabFactory.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Engine/Core/Scene.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <format>
#include <Engine/ECS/PrefabComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    PrefabComponentWidget::PrefabComponentWidget( const Assets::AssetManager* assetManager )
         : IComponentWidget( "Prefab" ), m_AssetManager( assetManager )
    {
    }

    void PrefabComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* scene )
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
                displayName = std::format( "Missing ({})", static_cast<uint64_t>( prefab.Prefab ) );
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

        // ── Streaming (WP21-WHOLE; UE Level Instance "Desired Runtime Behavior") ──
        // How this instance streams in a partitioned world: as its prefab says, or this instance's own override.
        // Partitioned: the body streams cell by cell; Standalone: the instance streams whole (one cell, one HLOD
        // source) and moving its root carries the body. The cook and Play read the saved record (PrefabExpansion).
        {
            const Assets::PrefabRuntimeBehavior prefabSays =
                 asset ? asset->GetRuntimeBehavior() : Assets::PrefabRuntimeBehavior::Partitioned;
            const std::string asPrefab =
                 asset ? std::format( "As prefab ({})", Assets::PrefabRuntimeBehaviorName( prefabSays ) )
                       : std::string( "As prefab" );
            const std::array<const char*, 3> options{ asPrefab.c_str(), "Partitioned", "Standalone" };
            int                              chosen = !prefab.RuntimeBehavior.has_value() ? 0
                                                      : *prefab.RuntimeBehavior == Assets::PrefabRuntimeBehavior::Partitioned ? 1
                                                                                                                              : 2;
            Utils::ImGuiUtilities::BeginPropertyRow( "Streaming" );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::Combo( "##PrefabRuntimeBehavior", &chosen, options.data(),
                               static_cast<int>( options.size() ) ) )
            {
                if ( chosen == 0 )
                    prefab.RuntimeBehavior.reset();
                else
                    prefab.RuntimeBehavior = chosen == 1 ? Assets::PrefabRuntimeBehavior::Partitioned
                                                         : Assets::PrefabRuntimeBehavior::Standalone;
                if ( scene != nullptr )
                    scene->MarkModifiedSubtree( entity );
            }
            Utils::ImGuiUtilities::Tooltip(
                 "How this instance streams in a partitioned world. Partitioned: each part "
                 "streams by its own bounds. Standalone: the whole instance streams as one "
                 "(one cell, one HLOD), and moving its root moves all of it." );
            Utils::ImGuiUtilities::EndPropertyRow();
        }

        // ── Select prefab by path ─────────────────────────────────────────
        ImGui::TextUnformatted( "Override Path" );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - 60.0f );
        Utils::ImGuiUtilities::InputText( m_SelectPathBuf, "##PrefabSelectPath" );
        ImGui::SameLine();
        if ( ImGui::Button( "Load" ) && !m_SelectPathBuf.empty() )
        {
            auto found = const_cast<Assets::AssetManager*>( m_AssetManager )
                              ->CreateAsset<Assets::PrefabAsset>( m_SelectPathBuf );
            if ( found )
            {
                found->Load();
                prefab.Prefab = found->GetMetadata().Handle;
                if ( scene != nullptr )
                    scene->MarkModifiedSubtree( entity );
            }
        }

        ImGui::Separator();

        // ── Edit in place (Commands::BeginPrefabEdit) ─────────────────────
        // The body is edited where this instance stands, the rest of the world locked; Commit writes the file and
        // rebuilds every instance with its own overrides, Discard puts this one back. One session at a time.
        if ( valid && entity.HasComponent<ECS::UUIDComponent>() )
        {
            const Common::UUID                id      = entity.GetComponent<ECS::UUIDComponent>().UUID;
            const std::optional<Common::UUID> editing = Commands::PrefabEditRoot();
            if ( !editing.has_value() )
            {
                if ( ImGui::Button( ICON_MDI_PENCIL " Edit in Place" ) )
                {
                    if ( const auto begun = Commands::BeginPrefabEdit( id ); !begun )
                        LOG_ERROR( "{}", begun.GetError() );
                    Utils::ImGuiUtilities::PopID();
                    return; // the instance was rebuilt as the bare body; this entity handle is gone
                }
                Utils::ImGuiUtilities::Tooltip(
                     "Edit the prefab's body here; the rest of the world is locked until Commit or Discard" );
            }
            else if ( *editing == id )
            {
                if ( ImGui::Button( ICON_MDI_CHECK " Commit" ) )
                {
                    if ( const auto committed = Commands::CommitPrefabEdit(); !committed )
                        LOG_ERROR( "{}", committed.GetError() );
                    Utils::ImGuiUtilities::PopID();
                    return;
                }
                Utils::ImGuiUtilities::Tooltip( "Write the body to the prefab and rebuild every instance (one undo step)" );
                ImGui::SameLine();
                if ( ImGui::Button( ICON_MDI_CLOSE " Discard" ) )
                {
                    if ( const auto discarded = Commands::DiscardPrefabEdit(); !discarded )
                        LOG_ERROR( "{}", discarded.GetError() );
                    Utils::ImGuiUtilities::PopID();
                    return;
                }
                Utils::ImGuiUtilities::Tooltip( "Put this instance back as it was; the prefab file is not touched" );
            }
            ImGui::Separator();
        }

        // ── Packed (UE Packed Level Actor): the mode, and the switch ─────
        const auto* packedInstance =
             entity.GetRegistry()->try_get<::Desert::Runtime::Factory::PackedPrefabComponent>(
                  entity.GetHandle() );
        const Common::UUID instanceId = entity.HasComponent<ECS::UUIDComponent>()
                                             ? entity.GetComponent<ECS::UUIDComponent>().UUID
                                             : Common::UUID::Null();
        if ( packedInstance != nullptr )
        {
            ImGui::TextUnformatted( std::format( "Packed: {} ISM", packedInstance->Groups.size() ).c_str() );
            if ( packedInstance->Form )
                Utils::ImGuiUtilities::Tooltip(
                     std::format( "{} instances in {} instanced draw(s), packed from source checksum {}",
                                  ::Desert::Assets::PackedInstanceCount( *packedInstance->Form ),
                                  packedInstance->Groups.size(), packedInstance->Form->SourceHash )
                          .c_str() );
            if ( valid && asset->GetPacked() && asset->GetPacked()->Refused.has_value() )
                ImGui::TextWrapped( "The prefab changed and no longer packs: %s",
                                    asset->GetPacked()->Refused->c_str() );
            if ( ImGui::Button( "Unpack to edit" ) )
            {
                if ( const auto switched = Commands::SetPrefabInstancesPacked( { instanceId }, false ); !switched )
                    LOG_ERROR( "[Prefab] Unpack refused: {}", switched.GetError() );
                Utils::ImGuiUtilities::PopID();
                return; // the instance was rebuilt; this entity handle is gone
            }
            Utils::ImGuiUtilities::Tooltip(
                 "Every entity of the prefab again, to edit the source in place; commit it "
                 "and every packed instance is rebuilt" );
        }
        else if ( valid )
        {
            const auto form = asset->GetPacked();
            if ( form && form->Refused.has_value() )
                ImGui::TextWrapped( "Cannot pack: %s", form->Refused->c_str() );
            else if ( ImGui::Button( "Pack" ) )
            {
                if ( const auto switched = Commands::SetPrefabInstancesPacked( { instanceId }, true ); !switched )
                    LOG_ERROR( "[Prefab] Pack refused: {}", switched.GetError() );
                Utils::ImGuiUtilities::PopID();
                return; // the instance was rebuilt; this entity handle is gone
            }
        }

        // ── Action buttons ────────────────────────────────────────────────
        if ( valid && packedInstance == nullptr )
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
                    // The streaming override went into the prefab with the rest (CreateFromEntity); the instance
                    // follows the prefab again.
                    if ( prefab.RuntimeBehavior.has_value() )
                    {
                        prefab.RuntimeBehavior.reset();
                        if ( scene != nullptr )
                            scene->MarkModifiedSubtree( entity );
                    }
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

            if ( ImGui::Button( ICON_MDI_PACKAGE_VARIANT_CLOSED " Break" ) )
            {
                // The Outliner's Break (one level): one route, one undo step.
                const auto broken = Commands::BreakPrefabInstance( entity.GetComponent<ECS::UUIDComponent>().UUID,
                                                                   Commands::BreakDepth::OneLevel );
                if ( !broken.IsSuccess() )
                    LOG_ERROR( "[Prefab] {}", broken.GetError() );
                Utils::ImGuiUtilities::PopID();
                return; // the link is gone (or the refusal is logged); stop rendering this widget
            }
            Utils::ImGuiUtilities::Tooltip(
                 "Break the prefab link one level: this instance's entities become regular "
                 "objects; prefabs nested in it stay instances (the Outliner's Break Prefab "
                 "(all levels) breaks those too)" );

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

#include "ImportOptionsDialog.hpp"

#include "ImportManager.hpp"

#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Json/Json.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <ImGui/imgui.h>

#include <array>
#include <cmath>
#include <deque>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor::ImportOptions
{
    namespace
    {
        namespace Ser = Assets::Serialization;

        constexpr const char* kWindowTitle = "Import Options###ImportOptionsWindow";

        // The window's state: the sources waiting, and the options shown (seeded from the project's last
        // confirmed ones when the window opens for a queue, then carried from source to source like UE's).
        struct QueuedSource
        {
            std::filesystem::path              Source;
            std::vector<std::function<void()>> OnImported; // run after it imported (Request's continuations)
        };
        struct WindowState
        {
            std::deque<QueuedSource>     Queue;
            Assets::SourceImportSettings Shown;
            bool                         Open = false;
            // The shown source was answered (a button or the palette): the modal closes on its next draw, which
            // is the only place ImGui lets it close.
            bool Answered = false;
        };

        WindowState& Window()
        {
            static WindowState s_State;
            return s_State;
        }

        // The Details section's edits in progress, by source: read from the record when the section first
        // shows the source, dropped by Reimport (the record then states them) and by Revert.
        struct SectionEdit
        {
            Assets::SourceImportSettings Recorded; // what the record states (read once, not every frame)
            Assets::SourceImportSettings Edit;     // what the section shows
        };
        std::unordered_map<std::string, SectionEdit>& Edits()
        {
            static std::unordered_map<std::string, SectionEdit> s_Edits;
            return s_Edits;
        }

        constexpr std::array<Assets::MeshSourceUpAxis, 3> kAxes = {
             Assets::MeshSourceUpAxis::FromFile, Assets::MeshSourceUpAxis::Y, Assets::MeshSourceUpAxis::Z };
        constexpr std::array<const char*, 3> kAxisLabels     = { "From File", "Y Up", "Z Up (convert to Y)" };
        constexpr std::array<Assets::MeshLodPolicy, 2> kLods = { Assets::MeshLodPolicy::Generate,
                                                                 Assets::MeshLodPolicy::None };
        constexpr std::array<const char*, 2> kLodLabels = { "Generate (authored, else simplified)", "LOD 0 only" };

        // EVERY ENTITY THAT DRAWS A REIMPORTED MESH DRAWS THE NEW CONTENT FROM THE NEXT FRAME (UE:
        // FReimportManager
        // -> PostReimport recreates the render resources). A loaded mesh drops its parsed payload and its built
        // GPU mesh; the next Get re-reads the file through the path a first use takes (as a committed modeling
        // edit does, ModelingToolTargetAsset.cpp). A mesh nobody loaded has nothing to refresh. A node mesh that
        // this import no longer writes (Combine Meshes switched) is not deleted: entities placed from it keep it,
        // as UE keeps the old assets until they are deleted by hand.
        void RefreshLoadedMeshes( const std::vector<std::filesystem::path>& written )
        {
            auto* service = Runtime::ResourceRegistry::GetMeshService();
            for ( const std::filesystem::path& path : written )
            {
                const Assets::AssetHandle handle = Assets::AssetHandle::FromCookedPath( path );
                if ( !service->HasAsset( handle ) )
                    continue;
                if ( auto* asset = service->GetAsset( handle ) )
                    if ( const auto unloaded = asset->Unload(); !unloaded )
                    {
                        LOG_ERROR( "[Import] '{}' was reimported but the loaded mesh was not reset: {}",
                                   path.generic_string(), unloaded.GetError() );
                        continue;
                    }
                (void)service->EvictBuilt( handle );
            }
        }

        // THE SKELETON AND THE CLIPS A REIMPORT REWROTE ARE RE-READ IN PLACE (UE: Reimport updates the skeleton
        // and the animation sequences with the mesh). In place because their consumers hold the asset itself: a
        // skinned mesh caches its rig asset (SkinnedMeshAsset's skeleton dependency, the MeshService entry's
        // Rig), an Animator binds the clip's AnimationClip, which Load rebuilds at the same address and stamps
        // with a new track revision. Every live manager that loaded one refreshes it; one nobody loaded has
        // nothing to refresh.
        template <typename AssetType>
        void ReloadLoaded( const std::vector<std::filesystem::path>& written )
        {
            for ( const std::filesystem::path& path : written )
                for ( Assets::AssetManager* manager : Assets::AssetManager::LiveManagers() )
                {
                    const auto asset = manager->FindByPath<AssetType>( path );
                    if ( !asset || !asset->IsReadyForUse() )
                        continue;
                    if ( const auto unloaded = asset->Unload(); !unloaded )
                    {
                        LOG_ERROR( "[Import] '{}' was reimported but the loaded asset was not reset: {}",
                                   path.generic_string(), unloaded.GetError() );
                        continue;
                    }
                    if ( const auto loaded = asset->Load(); !loaded )
                        LOG_ERROR( "[Import] '{}' was reimported but not read again: {}", path.generic_string(),
                                   loaded.GetError() );
                }
        }

        bool ImportOne( const std::filesystem::path& source, const Assets::SourceImportSettings& settings )
        {
            const ImportOutcome outcome = SharedImporter().ImportWithSettings( source, settings );
            // The rig first: the skinned meshes rebuilt below read it.
            ReloadLoaded<Assets::SkeletonAsset>( outcome.WrittenSkeletons );
            ReloadLoaded<Assets::AnimationAsset>( outcome.WrittenClips );
            RefreshLoadedMeshes( outcome.WrittenMeshes );
            if ( outcome.Verdict == CookVerdict::Failed || outcome.Verdict == CookVerdict::NotCookable )
            {
                LOG_ERROR( "[Import] '{}' was not imported with the chosen options (see the error above)",
                           source.generic_string() );
                return false;
            }
            return true;
        }
    } // namespace

    ImportManager& SharedImporter()
    {
        static ImportManager s_Importer;
        return s_Importer;
    }

    void Request( const std::filesystem::path& source, std::function<void()> onImported )
    {
        WindowState& w = Window();
        for ( auto& queued : w.Queue )
            if ( queued.Source == source )
            {
                if ( onImported )
                    queued.OnImported.push_back( std::move( onImported ) );
                return;
            }
        QueuedSource entry{ source, {} };
        if ( onImported )
            entry.OnImported.push_back( std::move( onImported ) );
        w.Queue.push_back( std::move( entry ) );
    }

    Common::BoolResultStr ConfirmImport( const bool all )
    {
        WindowState& w = Window();
        if ( w.Queue.empty() || w.Answered )
            return Common::MakeError<bool>( "no source waits in the Import Options window" );
        if ( auto saved = SaveLastUsed( w.Shown ); !saved )
            LOG_ERROR( "[Import] the options were not remembered for this project: {}", saved.GetError() );
        const std::size_t count = all ? w.Queue.size() : 1;
        for ( std::size_t i = 0; i < count && !w.Queue.empty(); ++i )
        {
            // Popped before its continuations run: a continuation may queue again (a drop of another new file).
            QueuedSource entry = std::move( w.Queue.front() );
            w.Queue.pop_front();
            if ( ImportOne( entry.Source, w.Shown ) )
                for ( const auto& then : entry.OnImported )
                    then();
        }
        w.Answered = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr CancelImport()
    {
        WindowState& w = Window();
        if ( w.Queue.empty() || w.Answered )
            return Common::MakeError<bool>( "no source waits in the Import Options window" );
        LOG_INFO( "[Import] '{}' was not imported (Import Options cancelled)",
                  w.Queue.front().Source.generic_string() );
        w.Queue.pop_front();
        w.Answered = true;
        return BOOLSUCCESS;
    }

    bool Pending()
    {
        return !Window().Queue.empty();
    }

    std::filesystem::path LastUsedPath()
    {
        const auto& root = Common::Constants::Path::CurrentProjectRoot().ProjectDir;
        return ( root.empty() ? std::filesystem::path( "Saved" ) : root / "Saved" ) / "ImportOptions.json";
    }

    Common::ResultStr<std::optional<Assets::SourceImportSettings>> LoadLastUsed()
    {
        using Result = std::optional<Assets::SourceImportSettings>;
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( LastUsedPath(), ec ) )
            return Common::MakeSuccess( Result{} );
        const auto read = Common::Json::ReadFile<Ser::SourceImportSettingsText>( LastUsedPath() );
        if ( !read )
            return Common::MakeError<Result>( read.GetError() );
        auto settings = Ser::ImportSettingsFromText( read.GetValue() );
        if ( !settings )
            return Common::MakeFormattedError<Result>( "'{}': {}", LastUsedPath().string(), settings.GetError() );
        return Common::MakeSuccess( Result{ settings.ExtractValue() } );
    }

    Common::BoolResultStr SaveLastUsed( const Assets::SourceImportSettings& settings )
    {
        std::error_code ec;
        std::filesystem::create_directories( LastUsedPath().parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "'{}' could not be created: {}",
                                                     LastUsedPath().parent_path().string(), ec.message() );
        return Common::Json::WriteFileAtomic( LastUsedPath(), Ser::ImportSettingsToText( settings ) );
    }

    bool DrawImportSettingsFields( Assets::SourceImportSettings& settings )
    {
        using UI     = Utils::ImGuiUtilities;
        bool changed = false;
        ImGui::PushID( "ImportSettingsFields" );

        UI::BeginPropertyRow( "Combine Meshes", "Off (UE's default): every mesh-bearing node of the file becomes "
                                                "its own static mesh. On: the whole file is one static mesh." );
        changed |= ImGui::Checkbox( "##CombineMeshes", &settings.CombineMeshes );
        UI::EndPropertyRow();

        UI::BeginPropertyRow( "Uniform Scale", "Applied on import: source units to centimetres." );
        float scale = settings.Mesh.UniformScale;
        if ( ImGui::DragFloat( "##UniformScale", &scale, 0.01f, 0.001f, 1000.0f, "%.3f" ) &&
             std::isfinite( scale ) && scale > 0.0f )
        {
            settings.Mesh.UniformScale = scale;
            changed                    = true;
        }
        UI::EndPropertyRow();

        UI::BeginPropertyRow( "Up Axis", "From File keeps the exporter's own axis conversion; Z Up rotates +Z to "
                                         "the engine's +Y." );
        int axis = 0;
        for ( std::size_t i = 0; i < kAxes.size(); ++i )
            if ( kAxes[i] == settings.Mesh.UpAxis )
                axis = static_cast<int>( i );
        if ( ImGui::Combo( "##UpAxis", &axis, kAxisLabels.data(), static_cast<int>( kAxisLabels.size() ) ) )
        {
            settings.Mesh.UpAxis = kAxes[static_cast<std::size_t>( axis )];
            changed              = true;
        }
        UI::EndPropertyRow();

        UI::BeginPropertyRow( "LODs", "Generate: the file's authored LODs, simplified ones otherwise. LOD 0 only: "
                                      "no LOD chain." );
        int lod = settings.Mesh.LodPolicy == Assets::MeshLodPolicy::None ? 1 : 0;
        if ( ImGui::Combo( "##Lods", &lod, kLodLabels.data(), static_cast<int>( kLodLabels.size() ) ) )
        {
            settings.Mesh.LodPolicy = kLods[static_cast<std::size_t>( lod )];
            changed                 = true;
        }
        UI::EndPropertyRow();

        ImGui::PopID();
        return changed;
    }

    void DrawWindow()
    {
        WindowState& w = Window();
        if ( !w.Open )
        {
            w.Answered = false;
            if ( w.Queue.empty() )
                return;
            // A new queue starts from the project's last confirmed options (UE: the import UI's saved config).
            auto last = LoadLastUsed();
            if ( !last )
                LOG_ERROR( "[Import] {}; the window starts from the defaults", last.GetError() );
            w.Shown = last && last.GetValue() ? *last.GetValue() : Assets::SourceImportSettings{};
            ImGui::OpenPopup( kWindowTitle );
            w.Open = true;
        }

        ImGui::SetNextWindowSize( ImVec2( 520.0f, 0.0f ), ImGuiCond_Appearing );
        if ( !ImGui::BeginPopupModal( kWindowTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            w.Open = false;
            return;
        }
        // Answered from the palette since the last draw: close here, where ImGui allows it. The next source, if
        // any, opens its own window on the next frame, from the options just confirmed.
        if ( w.Answered || w.Queue.empty() )
        {
            ImGui::CloseCurrentPopup();
            w.Open = false;
            ImGui::EndPopup();
            return;
        }

        const std::filesystem::path source = w.Queue.front().Source;
        ImGui::TextUnformatted( ICON_MDI_FILE_IMPORT_OUTLINE "  Static Mesh import" );
        ImGui::Separator();
        ImGui::TextUnformatted( std::format( "File: {}", source.filename().string() ).c_str() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", source.generic_string().c_str() );
        if ( w.Queue.size() > 1 )
            ImGui::TextDisabled(
                 "%s", std::format( "{} more file(s) wait after this one", w.Queue.size() - 1 ).c_str() );
        ImGui::Spacing();

        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_SHAPE "  Mesh" ) )
        {
            Utils::ImGuiUtilities::ResetPropertyRows();
            (void)DrawImportSettingsFields( w.Shown );
        }
        ImGui::Spacing();
        ImGui::Separator();

        if ( ImGui::Button( "Import", ImVec2( 110.0f, 0.0f ) ) )
            (void)ConfirmImport( false );
        ImGui::SameLine();
        if ( ImGui::Button( "Import All", ImVec2( 110.0f, 0.0f ) ) )
            (void)ConfirmImport( true );
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            (void)CancelImport();
        if ( w.Answered )
        {
            ImGui::CloseCurrentPopup();
            w.Open = false;
        }
        ImGui::EndPopup();
    }

    std::optional<std::filesystem::path> ImportSourceOfMeshAsset( const std::filesystem::path& assetPath )
    {
        if ( auto beside = Common::Content::MeshSourceBeside( assetPath ) )
            return beside;
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( assetPath, ec ) )
            return std::nullopt;
        const auto asset = Assets::ReadMeshSourceAssetFile( assetPath );
        if ( !asset || asset.GetValue().Import.Provenance != Assets::MeshSourceProvenance::Imported ||
             asset.GetValue().Import.SourceFile.empty() )
            return std::nullopt;
        std::filesystem::path source = Common::AssetHandle::PathForStableKey( asset.GetValue().Import.SourceFile );
        if ( !std::filesystem::is_regular_file( source, ec ) )
            return std::nullopt;
        return source;
    }

    void DrawImportSettingsSection( const std::filesystem::path& assetPath )
    {
        const auto source = ImportSourceOfMeshAsset( assetPath );
        if ( !source )
            return;
        const std::string key   = source->generic_string();
        auto&             edits = Edits();
        auto              it    = edits.find( key );
        if ( it == edits.end() )
        {
            auto recorded = Ser::ReadImportRecordSettings( *source );
            if ( !recorded )
            {
                if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FILE_IMPORT_OUTLINE "  Import Settings" ) )
                    ImGui::TextWrapped( "%s", recorded.GetError().c_str() );
                return;
            }
            it = edits.emplace( key, SectionEdit{ recorded.GetValue(), recorded.GetValue() } ).first;
        }

        if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FILE_IMPORT_OUTLINE "  Import Settings" ) )
            return;
        ImGui::PushID( key.c_str() );
        Utils::ImGuiUtilities::ResetPropertyRows();
        Utils::ImGuiUtilities::BeginPropertyRow( "Source File" );
        ImGui::TextUnformatted( source->filename().string().c_str() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", key.c_str() );
        Utils::ImGuiUtilities::EndPropertyRow();
        (void)DrawImportSettingsFields( it->second.Edit );

        const bool edited = it->second.Recorded != it->second.Edit;
        if ( ImGui::Button( ICON_MDI_RELOAD "  Reimport" ) )
        {
            ImportOne( *source, it->second.Edit );
            edits.erase( it ); // the record states what was imported now; the next frame reads it
            ImGui::PopID();
            return;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Imports '%s' again with these options%s", source->filename().string().c_str(),
                               edited ? " (changed, not applied yet)" : "" );
        if ( edited )
        {
            ImGui::SameLine();
            if ( ImGui::Button( "Revert" ) )
                it->second.Edit = it->second.Recorded;
        }
        ImGui::PopID();
    }
} // namespace Desert::Editor::ImportOptions

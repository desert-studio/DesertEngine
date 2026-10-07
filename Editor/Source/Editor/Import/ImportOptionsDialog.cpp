#include "ImportOptionsDialog.hpp"

#include "CookPaths.hpp"
#include "ImportManager.hpp"
#include "ImportedAssetSource.hpp"
#include "ImportSettingsEdits.hpp"

#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <ImGui/imgui.h>

#include <array>
#include <algorithm>
#include <cctype>
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
            // What the file holds, read once when it is first shown (the window's title and sections).
            std::optional<Common::ResultStr<ImportContentKind>> Kind;
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

        // Which fields a record's Kind shows: StaticMesh, SkinnedMesh -> SkeletalMesh, Skeleton / Animation ->
        // Animation (ReadImportRecordKind admits only the kinds an import writes).
        ImportContentKind ImportKindOf( const Common::Content::ContentKind kind )
        {
            switch ( kind )
            {
                case Common::Content::ContentKind::SkinnedMesh:
                    return ImportContentKind::SkeletalMesh;
                case Common::Content::ContentKind::Skeleton:
                case Common::Content::ContentKind::Animation:
                    return ImportContentKind::Animation;
                default:
                    return ImportContentKind::StaticMesh;
            }
        }

        // The Details section's working copy of the mesh asset at @p assetPath's source (ImportSettingsEdits).
        Common::ResultStr<ImportSettingsEdit*> SectionEditOf( const std::filesystem::path& assetPath )
        {
            const auto source = ImportSourceOfAsset( assetPath );
            if ( !source )
                return Common::MakeFormattedError<ImportSettingsEdit*>( "'{}' has no import source",
                                                                        assetPath.generic_string() );
            return EditOf( *source );
        }

        constexpr std::array<Assets::MeshSourceUpAxis, 3> kAxes = {
             Assets::MeshSourceUpAxis::FromFile, Assets::MeshSourceUpAxis::Y, Assets::MeshSourceUpAxis::Z };
        constexpr std::array<const char*, 3> kAxisLabels     = { "From File", "Y Up", "Z Up (convert to Y)" };
        constexpr std::array<Assets::MeshLodPolicy, 2> kLods = { Assets::MeshLodPolicy::Generate,
                                                                 Assets::MeshLodPolicy::None };
        constexpr std::array<const char*, 2> kLodLabels = { "Generate (authored, else simplified)", "LOD 0 only" };

        // The window's title and its options section, by what the file holds (UE: Static Mesh / Skeletal Mesh /
        // Animation import).
        const char* KindTitle( const ImportContentKind kind )
        {
            switch ( kind )
            {
                case ImportContentKind::StaticMesh:
                    return ICON_MDI_FILE_IMPORT_OUTLINE "  Static Mesh import";
                case ImportContentKind::SkeletalMesh:
                    return ICON_MDI_FILE_IMPORT_OUTLINE "  Skeletal Mesh import";
                case ImportContentKind::Animation:
                    return ICON_MDI_FILE_IMPORT_OUTLINE "  Animation import";
            }
            return "";
        }
        const char* KindSection( const ImportContentKind kind )
        {
            switch ( kind )
            {
                case ImportContentKind::StaticMesh:
                    return ICON_MDI_SHAPE "  Mesh";
                case ImportContentKind::SkeletalMesh:
                    return ICON_MDI_SHAPE "  Skeletal Mesh";
                case ImportContentKind::Animation:
                    return ICON_MDI_SHAPE "  Animation";
            }
            return "";
        }

        // The window's shown options, for an edit from the palette: refused when no source waits.
        Common::ResultStr<Assets::SourceImportSettings*> ShownSettings()
        {
            WindowState& w = Window();
            if ( w.Queue.empty() || w.Answered )
                return Common::MakeError<Assets::SourceImportSettings*>(
                     "no source waits in the Import Options window" );
            return Common::MakeSuccess( &w.Shown );
        }

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
                // The row's handle, the number the entities hold (LoadedHandleOf: a `.skmesh` is known by its
                // GUID).
                const auto known = LoadedHandleOf( path );
                if ( !known )
                {
                    LOG_ERROR( "[Import] '{}' was reimported and its loaded mesh cannot be named: {}",
                               path.generic_string(), known.GetError() );
                    continue;
                }
                const Assets::AssetHandle handle = known.GetValue();
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
        // with a new track revision; SkeletonAsset::LoadFromFile rewrites its Skeleton at the same address and
        // a changed signature makes AnimationECSSystem rebuild the Animator. So it is `Load()` on the loaded
        // asset and NEVER `Unload()` first: that frees the Skeleton the meshes and Animators still point at.
        // Every live manager that loaded one refreshes it; one nobody loaded has nothing to refresh.
        template <typename AssetType>
        void ReloadLoaded( const std::vector<std::filesystem::path>& written )
        {
            for ( const std::filesystem::path& path : written )
                for ( Assets::AssetManager* manager : Assets::AssetManager::LiveManagers() )
                {
                    const auto asset = manager->FindByPath<AssetType>( path );
                    if ( !asset || !asset->IsReadyForUse() )
                        continue;
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

    Common::BoolResultStr SetUniformScale( Assets::SourceImportSettings& settings, const float scale )
    {
        if ( !std::isfinite( scale ) || scale <= 0.0f )
            return Common::MakeFormattedError<bool>( "Uniform Scale {} is not a finite number above zero", scale );
        settings.Mesh.UniformScale = scale;
        return BOOLSUCCESS;
    }

    void SetUpAxis( Assets::SourceImportSettings& settings, const Assets::MeshSourceUpAxis axis )
    {
        settings.Mesh.UpAxis = axis;
    }

    void SetCombineMeshes( Assets::SourceImportSettings& settings, const bool on )
    {
        settings.CombineMeshes = on;
    }

    Common::BoolResultStr SetShownUniformScale( const float scale )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        return SetUniformScale( *shown.GetValue(), scale );
    }

    Common::BoolResultStr SetShownUpAxis( const Assets::MeshSourceUpAxis axis )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        SetUpAxis( *shown.GetValue(), axis );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SetShownCombineMeshes( const bool on )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        SetCombineMeshes( *shown.GetValue(), on );
        return BOOLSUCCESS;
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
        QueuedSource entry{ source, {}, std::nullopt };
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
            const QueuedSource entry = std::move( w.Queue.front() );
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
        // The Skeleton choice belongs to the file it was made for (its record): a remembered one would import the
        // next, unrelated file onto it.
        Assets::SourceImportSettings remembered = settings;
        remembered.Skeleton.reset();
        return Common::Json::WriteFileAtomic( LastUsedPath(), Ser::ImportSettingsToText( remembered ) );
    }

    bool DrawImportSettingsFields( Assets::SourceImportSettings& settings, const ImportContentKind kind )
    {
        using UI     = Utils::ImGuiUtilities;
        bool changed = false;
        ImGui::PushID( "ImportSettingsFields" );

        // A skinned file is one skeletal mesh whatever its nodes (UE's Skeletal Mesh import has no Combine
        // Meshes).
        if ( kind == ImportContentKind::StaticMesh )
        {
            UI::BeginPropertyRow( "Combine Meshes",
                                  "Off (UE's default): every mesh-bearing node of the file "
                                  "becomes its own static mesh. On: the whole file is one static "
                                  "mesh." );
            bool combine = settings.CombineMeshes;
            if ( ImGui::Checkbox( "##CombineMeshes", &combine ) )
            {
                SetCombineMeshes( settings, combine );
                changed = true;
            }
            UI::EndPropertyRow();
        }

        UI::BeginPropertyRow( "Uniform Scale", "Applied on import: source units to centimetres." );
        float scale = settings.Mesh.UniformScale;
        if ( ImGui::DragFloat( "##UniformScale", &scale, 0.01f, 0.001f, 1000.0f, "%.3f" ) &&
             SetUniformScale( settings, scale ) )
            changed = true;
        UI::EndPropertyRow();

        UI::BeginPropertyRow( "Up Axis", "From File keeps the exporter's own axis conversion; Z Up rotates +Z to "
                                         "the engine's +Y." );
        int axis = 0;
        for ( std::size_t i = 0; i < kAxes.size(); ++i )
            if ( kAxes[i] == settings.Mesh.UpAxis )
                axis = static_cast<int>( i );
        if ( ImGui::Combo( "##UpAxis", &axis, kAxisLabels.data(), static_cast<int>( kAxisLabels.size() ) ) )
        {
            SetUpAxis( settings, kAxes[static_cast<std::size_t>( axis )] );
            changed = true;
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

        // UE's Skeleton field of the skeletal mesh / animation import: an existing skeleton the rig is imported
        // onto, or none (the default) - then the one skeleton whose bones state the rig's, else a new one
        // (ImportManager ExistingSkeletonFor). Listed from the registry's Skeleton rows, loading nothing.
        if ( kind != ImportContentKind::StaticMesh )
        {
            UI::BeginPropertyRow( "Skeleton",
                                  "None: the one registered skeleton with the file's bones, else a new "
                                  ".skeleton. A chosen skeleton must hold every bone the mesh and the "
                                  "clips need, or the import is refused." );
            const auto rows   = Assets::ContentRegistry::Rows( Common::Content::ContentKind::Skeleton );
            const auto nameOf = []( const Assets::ContentRegistry::PickerRow& row )
            { return row.DisplayName.empty() ? row.Path.stem().string() : row.DisplayName; };
            std::string preview = "None (new or matching skeleton)";
            if ( settings.Skeleton )
            {
                preview =
                     std::format( "Missing skeleton {}", Common::Content::AssetGuidToText( *settings.Skeleton ) );
                for ( const auto& row : rows )
                    if ( row.Guid && *row.Guid == *settings.Skeleton )
                        preview = nameOf( row );
            }
            if ( ImGui::BeginCombo( "##Skeleton", preview.c_str() ) )
            {
                if ( ImGui::Selectable( "None (new or matching skeleton)", !settings.Skeleton ) )
                {
                    settings.Skeleton.reset();
                    changed = true;
                }
                for ( const auto& row : rows )
                {
                    if ( !row.Guid )
                        continue;
                    ImGui::PushID( row.Key.c_str() );
                    const bool selected = settings.Skeleton && *settings.Skeleton == *row.Guid;
                    if ( ImGui::Selectable( nameOf( row ).c_str(), selected ) )
                    {
                        settings.Skeleton = *row.Guid;
                        changed           = true;
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "%s", row.Key.c_str() );
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            UI::EndPropertyRow();
        }

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
            w.Shown = Assets::SourceImportSettings{};
            if ( last )
                if ( const auto& saved = last.GetValue(); saved )
                    w.Shown = *saved;
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

        QueuedSource& shown = w.Queue.front();
        if ( !shown.Kind )
            shown.Kind = SharedImporter().ProbeContent( shown.Source );
        const std::filesystem::path source = shown.Source;
        if ( !*shown.Kind )
        {
            // Not readable: said in the window, and Import will fail with the same file named.
            ImGui::TextUnformatted( ICON_MDI_FILE_IMPORT_OUTLINE "  Import" );
            ImGui::TextWrapped( "%s", shown.Kind->GetError().c_str() );
        }
        else
            ImGui::TextUnformatted( KindTitle( shown.Kind->GetValue() ) );
        ImGui::Separator();
        ImGui::TextUnformatted( std::format( "File: {}", source.filename().string() ).c_str() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", source.generic_string().c_str() );
        if ( w.Queue.size() > 1 )
            ImGui::TextDisabled(
                 "%s", std::format( "{} more file(s) wait after this one", w.Queue.size() - 1 ).c_str() );
        ImGui::Spacing();

        const ImportContentKind kind = *shown.Kind ? shown.Kind->GetValue() : ImportContentKind::StaticMesh;
        if ( *shown.Kind && Utils::ImGuiUtilities::SectionHeader( KindSection( kind ) ) )
        {
            Utils::ImGuiUtilities::ResetPropertyRows();
            (void)DrawImportSettingsFields( w.Shown, kind );
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

    std::optional<std::filesystem::path> ImportSourceOfAsset( const std::filesystem::path& assetPath )
    {
        // A skinned import's file is traced to its source by what the import wrote (ImportedAssetSource::
        // SkinnedAssetSource): a clip's own `Import` (`<stem>_<clip>.anim` has no inverse), the record beside a
        // mesh or a rig (it states the extension the name cannot).
        if ( CookPaths::IsSkinnedAssetFile( assetPath ) )
        {
            const auto stated = ImportedAssetSource::SkinnedAssetSource( assetPath );
            if ( !stated )
                return std::nullopt;
            const std::optional<std::filesystem::path>& source = stated.GetValue();
            std::error_code                             ec;
            if ( !source.has_value() || !std::filesystem::is_regular_file( source.value(), ec ) )
                return std::nullopt;
            return source;
        }
        if ( auto beside = Common::Content::MeshSourceBeside( assetPath ) )
            return beside;
        if ( assetPath.extension() != ".stmesh" ) // only a static node mesh names its source inside (IMPT)
            return std::nullopt;
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

    Common::ResultStr<ImportContentKind> RecordedImportKind( const std::filesystem::path& source )
    {
        const auto kind = Ser::ReadImportRecordKind( source );
        if ( !kind )
            return Common::MakeError<ImportContentKind>( kind.GetError() );
        return Common::MakeSuccess( ImportKindOf( kind.GetValue() ) );
    }

    void DrawImportSettingsSection( const std::filesystem::path& assetPath )
    {
        const auto source = ImportSourceOfAsset( assetPath );
        if ( !source )
            return;
        const std::string key   = source->generic_string();
        const auto        found = EditOf( *source );
        if ( !found )
        {
            if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FILE_IMPORT_OUTLINE "  Import Settings" ) )
                ImGui::TextWrapped( "%s", found.GetError().c_str() );
            return;
        }
        ImportSettingsEdit& edit = *found.GetValue();

        if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FILE_IMPORT_OUTLINE "  Import Settings" ) )
            return;
        ImGui::PushID( key.c_str() );
        // The source is a FACT, not a setting (UE's AssetImportData "Source File"): its full path in the
        // read-only table, whose declared columns cannot clip it down to the extension.
        if ( Utils::ImGuiUtilities::BeginFactTable( "##ImportSource" ) )
        {
            std::error_code ec;
            const auto      full = std::filesystem::absolute( *source, ec );
            Utils::ImGuiUtilities::FactRow( "Source File",
                                            ( ec ? *source : full ).lexically_normal().generic_string() );
            Utils::ImGuiUtilities::EndFactTable();
        }
        Utils::ImGuiUtilities::ResetPropertyRows();
        (void)DrawImportSettingsFields( edit.Edit, ImportKindOf( edit.Kind ) );

        const bool edited = edit.Recorded != edit.Edit;
        if ( ImGui::Button( ICON_MDI_RELOAD "  Reimport" ) )
        {
            if ( const auto reimported = Reimport( assetPath ); !reimported )
                LOG_ERROR( "[Import] {}", reimported.GetError() );
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
                edit.Edit = edit.Recorded;
        }
        ImGui::PopID();
    }

    Common::BoolResultStr Reimport( const std::filesystem::path& assetPath )
    {
        const auto source = ImportSourceOfAsset( assetPath );
        if ( !source )
            return Common::MakeFormattedError<bool>( "'{}' has no import source to reimport from",
                                                     assetPath.generic_string() );
        // The ONE working copy, read again when the record on disk is newer than it (ImportSettingsEdits).
        const auto edit = EditOf( *source );
        if ( !edit )
            return Common::MakeError<bool>( edit.GetError() );
        const Assets::SourceImportSettings settings = edit.GetValue()->Edit;
        if ( !ImportOne( *source, settings ) )
            return Common::MakeFormattedError<bool>( "'{}' was not reimported (see the error above)",
                                                     source->generic_string() );
        DropEdit( *source ); // the record states what was imported now; the next ask reads it
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ReimportWithNewFile( const std::filesystem::path& assetPath,
                                               const std::filesystem::path& newFile )
    {
        const auto source = ImportSourceOfAsset( assetPath );
        if ( !source )
            return Common::MakeFormattedError<bool>( "'{}' has no import source to replace",
                                                     assetPath.generic_string() );
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( newFile, ec ) )
            return Common::MakeFormattedError<bool>( "'{}' is not a file", newFile.generic_string() );
        const auto lower = []( std::string text )
        {
            std::transform( text.begin(), text.end(), text.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return text;
        };
        if ( lower( newFile.extension().string() ) != lower( source->extension().string() ) )
            return Common::MakeFormattedError<bool>( "'{}' is a {} file; '{}' reimports from {} only",
                                                     newFile.generic_string(), newFile.extension().string(),
                                                     assetPath.generic_string(), source->extension().string() );
        if ( !std::filesystem::equivalent( newFile, *source, ec ) &&
             !std::filesystem::copy_file( newFile, *source, std::filesystem::copy_options::overwrite_existing,
                                          ec ) )
            return Common::MakeFormattedError<bool>( "'{}' could not replace '{}': {}", newFile.generic_string(),
                                                     source->generic_string(), ec.message() );
        return Reimport( assetPath );
    }

    Common::BoolResultStr SetSectionUniformScale( const std::filesystem::path& assetPath, const float scale )
    {
        const auto edit = SectionEditOf( assetPath );
        if ( !edit )
            return Common::MakeError<bool>( edit.GetError() );
        return SetUniformScale( edit.GetValue()->Edit, scale );
    }

    Common::BoolResultStr SetSectionUpAxis( const std::filesystem::path&   assetPath,
                                            const Assets::MeshSourceUpAxis axis )
    {
        const auto edit = SectionEditOf( assetPath );
        if ( !edit )
            return Common::MakeError<bool>( edit.GetError() );
        SetUpAxis( edit.GetValue()->Edit, axis );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::ImportOptions

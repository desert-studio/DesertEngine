#include "ImportOptionsDialog.hpp"

#include "ImportManager.hpp"

#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Json/Json.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <ImGui/imgui.h>

#include <array>
#include <cmath>
#include <deque>
#include <format>
#include <string>
#include <unordered_map>

namespace Desert::Editor::ImportOptions
{
    namespace
    {
        namespace Ser = Assets::Serialization;

        constexpr const char* kWindowTitle = "Import Options###ImportOptionsWindow";

        // The window's state: the sources waiting, and the options shown (seeded from the project's last
        // confirmed ones when the window opens for a queue, then carried from source to source like UE's).
        struct WindowState
        {
            std::deque<std::filesystem::path> Queue;
            Assets::SourceImportSettings      Shown;
            bool                              Open = false;
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

        void ImportOne( const std::filesystem::path& source, const Assets::SourceImportSettings& settings )
        {
            const CookVerdict verdict = SharedImporter().ImportWithSettings( source, settings );
            if ( verdict == CookVerdict::Failed || verdict == CookVerdict::NotCookable )
                LOG_ERROR( "[Import] '{}' was not imported with the chosen options (see the error above)",
                           source.generic_string() );
        }

        void ConfirmAndImport( const std::size_t count )
        {
            WindowState& w = Window();
            if ( auto saved = SaveLastUsed( w.Shown ); !saved )
                LOG_ERROR( "[Import] the options were not remembered for this project: {}", saved.GetError() );
            for ( std::size_t i = 0; i < count && !w.Queue.empty(); ++i )
            {
                ImportOne( w.Queue.front(), w.Shown );
                w.Queue.pop_front();
            }
        }
    } // namespace

    ImportManager& SharedImporter()
    {
        static ImportManager s_Importer;
        return s_Importer;
    }

    void Request( const std::filesystem::path& source )
    {
        WindowState& w = Window();
        for ( const auto& queued : w.Queue )
            if ( queued == source )
                return;
        w.Queue.push_back( source );
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
        if ( w.Queue.empty() )
            return;
        if ( !w.Open )
        {
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

        const std::filesystem::path& source = w.Queue.front();
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

        bool close = false;
        if ( ImGui::Button( "Import", ImVec2( 110.0f, 0.0f ) ) )
        {
            ConfirmAndImport( 1 );
            close = true;
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Import All", ImVec2( 110.0f, 0.0f ) ) )
        {
            ConfirmAndImport( w.Queue.size() );
            close = true;
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
        {
            LOG_INFO( "[Import] '{}' was not imported (Import Options cancelled)", source.generic_string() );
            w.Queue.pop_front();
            close = true;
        }
        if ( close )
        {
            // The next source, if any, opens its own window on the next frame, from the options just confirmed.
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

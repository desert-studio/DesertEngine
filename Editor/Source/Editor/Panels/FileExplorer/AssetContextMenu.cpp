#define NOMINMAX // engine headers below use std::min/max; keep the windows.h macros out

#include "AssetContextMenu.hpp"

#include <Editor/Core/AssetFileOps.hpp>
#include <Editor/Core/AssetReferences.hpp>
#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Import/ImportOptionsDialog.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserSelection.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserUtils.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Panels/FileExplorer/ThumbnailEditMode.hpp>
#include <Editor/Platform/DesktopPlatform.hpp>
#include <Editor/Widgets/ThumbnailEdit.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    // Port pattern: UE Editor/ContentBrowser/Private/AssetContextMenu.cpp (the menu built for the selection,
    // every action an execute on it) and SContentBrowser's rename / delete flow.
    AssetContextMenu::AssetContextMenu( ContentBrowserSelection& selection, ThumbnailEditMode& thumbnailEdit,
                                        Delegates delegates )
         : m_Selection( selection ), m_ThumbnailEdit( thumbnailEdit ), m_On( std::move( delegates ) )
    {
    }

    void AssetContextMenu::Draw( DirectoryInformation& entry, const DirectoryInformation* folder )
    {
        if ( !ImGui::BeginPopupContextItem( "##ItemContext" ) )
            return;

        // UE: a right click on an entry outside the selection selects it, so the menu's commands (which act on
        // the selection, as the palette's do) act on what was clicked.
        if ( !m_Selection.Contains( &entry ) )
            m_Selection.SelectOnly( entry );
        else
            m_Selection.SetCurrent( entry );
        const std::string name = std::filesystem::path( entry.AssetPath ).filename().string();
        ImGui::TextDisabled( "%s", name.c_str() );
        ImGui::Separator();

        if ( entry.IsFile )
        {
            CommandMenuItem( ContentBrowserCommand::Open, folder ); // default app for this file type
            CommandMenuItem( ContentBrowserCommand::ShowInExplorer, folder );
            CommandMenuItem( ContentBrowserCommand::OpenContainingFolder, folder );

            // Quick "Add to Scene" for prefabs (same instantiate path as dragging into the viewport).
            if ( entry.Type == FileType::Prefab && m_On.CanAddToScene() )
            {
                ImGui::Separator();
                if ( ImGui::MenuItem( "Add to Scene" ) )
                    m_On.OnAddToScene( entry.AssetPath );
            }

            // UE's Asset Actions > Reimport / Reimport with New File, for a mesh asset with an import source.
            if ( ImportOptions::ImportSourceOfAsset( entry.AssetPath ) )
            {
                ImGui::Separator();
                CommandMenuItem( ContentBrowserCommand::Reimport, folder );
                CommandMenuItem( ContentBrowserCommand::ReimportWithNewFile, folder );
            }

            // UE-style: use the current viewport view as this asset's thumbnail (frame it in the scene first).
            if ( ThumbnailProducers::CaptureKeyOf( entry.Type ) && m_ThumbnailEdit.CanCapture() )
            {
                ImGui::Separator();
                CommandMenuItem( ContentBrowserCommand::CaptureThumbnail, folder );
            }

            // UE "Edit Thumbnail": the tile itself becomes the orbit control. Offered only when the asset states
            // an orbit to edit (a model with no import record has none); the item says why when it cannot.
            if ( ThumbnailProducers::HasThumbnailOrbit( entry.Type ) )
            {
                ImGui::Separator();
                const std::optional<std::string> orbitFile = m_ThumbnailEdit.OrbitFileOf( entry );
                const auto                       stated =
                     orbitFile ? ThumbnailEdit::ReadOrbit( *orbitFile )
                                                     : Common::MakeError<Assets::ThumbnailOrbit>( "not imported: no picture to edit" );
                CommandMenuItem( ContentBrowserCommand::EditThumbnail, folder,
                                 m_ThumbnailEdit.IsEditing( entry.AssetPath ), stated.IsSuccess() );
                if ( !stated && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                    ImGui::SetTooltip( "%s", stated.GetError().c_str() );
            }
        }
        else
        {
            CommandMenuItem( ContentBrowserCommand::Open, folder );
            CommandMenuItem( ContentBrowserCommand::ShowInExplorer, folder );
            if ( ImGui::MenuItem( EditorPreferences::IsFavouriteFolder( entry.AssetPath ) ? "Remove from Favorites"
                                                                                          : "Add to Favorites" ) )
                EditorPreferences::ToggleFavouriteFolder( entry.AssetPath );
        }

        ImGui::Separator();
        if ( ImGui::MenuItem( "Copy Path" ) )
            ImGui::SetClipboardText(
                 std::filesystem::absolute( entry.AssetPath ).make_preferred().string().c_str() );
        if ( ImGui::MenuItem( "Copy Name" ) )
            ImGui::SetClipboardText( name.c_str() );

        // ---- File operations (cross-platform, on the selection) ----
        const std::vector<std::string> sel   = m_Selection.Paths();
        const size_t                   count = sel.size();

        ImGui::Separator();
        if ( count <= 1 && ImGui::MenuItem( "Rename", "F2" ) )
        {
            m_Selection.SetCurrent( entry );
            static_cast<void>( Rename() );
        }
        if ( ImGui::MenuItem( count > 1 ? "Duplicate selection" : "Duplicate" ) )
        {
            for ( const auto& p : sel )
            {
                std::string np, err;
                if ( !AssetFileOps::Duplicate( p, np, err ) )
                    m_On.OnStatus( "Duplicate failed: " + err );
            }
            m_On.OnRefresh();
        }
        if ( ImGui::MenuItem( "Cut", "Ctrl+X" ) )
            m_Selection.Copy( true );
        if ( ImGui::MenuItem( "Copy", "Ctrl+C" ) )
            m_Selection.Copy( false );
        ImGui::Separator();
        if ( ImGui::MenuItem( count > 1 ? "Delete selection" : "Delete", "Del" ) )
            RequestDelete( sel, true );

        ImGui::EndPopup();
    }

    void AssetContextMenu::RequestDelete( std::vector<std::string> paths, bool scanReferencers )
    {
        m_PendingDeleteList = std::move( paths );
        m_DeleteReferencers.clear();
        if ( scanReferencers )
        {
            // Safe delete: warn if any file in the selection is still referenced (best-effort text scan).
            AssetReferenceIndex idx;
            BuildProjectAssetReferenceIndex( idx );
            for ( const auto& p : m_PendingDeleteList )
            {
                std::error_code   ec;
                const std::string rel =
                     std::filesystem::relative( p, Common::Constants::Path::ASSETS_PATH, ec ).generic_string();
                for ( const auto& r : idx.ReferencersOf( rel ) )
                    m_DeleteReferencers.push_back( r );
            }
        }
        m_ShowDeleteConfirm = true;
    }

    void AssetContextMenu::HandleShortcuts( const DirectoryInformation* folder )
    {
        if ( !ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) || !m_Selection.Current() ||
             ImGui::GetIO().WantTextInput )
            return;
        if ( ImGui::IsKeyPressed( ImGuiKey_F2, false ) )
            static_cast<void>( Rename() );
        if ( ImGui::IsKeyPressed( ImGuiKey_Delete, false ) )
            RequestDelete( m_Selection.Paths(), false );
        const bool mod = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
        if ( mod && ImGui::IsKeyPressed( ImGuiKey_C, false ) )
            m_Selection.Copy( false );
        if ( mod && ImGui::IsKeyPressed( ImGuiKey_X, false ) )
            m_Selection.Copy( true );
        if ( mod && ImGui::IsKeyPressed( ImGuiKey_V, false ) )
            Paste( folder );
    }

    void AssetContextMenu::DrawPopups()
    {
        // ---- Rename ----
        if ( m_ShowRenamePopup )
        {
            ImGui::OpenPopup( "Rename##assetRename" );
            m_ShowRenamePopup = false;
        }
        if ( ImGui::BeginPopupModal( "Rename##assetRename", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "New name:" );
            ImGui::SetNextItemWidth( 320.0f );
            const bool submit =
                 ImGui::InputText( "##renameField", m_RenameBuf, sizeof( m_RenameBuf ),
                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll );
            if ( ImGui::IsWindowAppearing() )
                ImGui::SetKeyboardFocusHere( -1 );

            if ( !m_RenameReferrers.empty() )
            {
                ImGui::Spacing();
                ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ),
                                    "%zu asset(s) reference it and keep loading through a redirector:",
                                    m_RenameReferrers.size() );
                ImGui::BeginChild( "##renamerefs", ImVec2( 360.0f, 90.0f ), true );
                for ( const auto& r : m_RenameReferrers )
                    ImGui::BulletText( "%s", r.c_str() );
                ImGui::EndChild();
            }

            if ( ImGui::Button( "Rename", ImVec2( 110.0f, 0.0f ) ) || submit )
            {
                const std::filesystem::path from = m_RenamePath;
                std::string                 err;
                if ( m_RenameBuf[0] == '\0' )
                    m_On.OnStatus( "Rename failed: the name cannot be empty" );
                else if ( from.filename() != std::filesystem::path( m_RenameBuf ) )
                {
                    if ( ContentBrowserUtils::MoveOrRename( m_RenamePath, from.parent_path() / m_RenameBuf,
                                                            "Rename", err ) )
                        m_On.OnRefresh();
                    else
                        m_On.OnStatus( "Rename failed: " + err );
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ---- Delete (confirm, with a reference warning) ----
        if ( m_ShowDeleteConfirm )
        {
            ImGui::OpenPopup( "Delete?##assetDelete" );
            m_ShowDeleteConfirm = false;
        }
        if ( ImGui::BeginPopupModal( "Delete?##assetDelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            if ( m_PendingDeleteList.size() == 1 )
                ImGui::Text( "Delete \"%s\"?",
                             std::filesystem::path( m_PendingDeleteList.front() ).filename().string().c_str() );
            else
                ImGui::Text( "Delete %zu items?", m_PendingDeleteList.size() );

            if ( !m_DeleteReferencers.empty() )
            {
                ImGui::Spacing();
                ImGui::TextColored(
                     ImVec4( 1.0f, 0.6f, 0.3f, 1.0f ),
                     "Warning: %zu asset(s) still reference the selection:", m_DeleteReferencers.size() );
                ImGui::BeginChild( "##delrefs", ImVec2( 360.0f, 90.0f ), true );
                for ( const auto& r : m_DeleteReferencers )
                    ImGui::BulletText( "%s", r.c_str() );
                ImGui::EndChild();
            }
            ImGui::Spacing();

            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.6f, 0.15f, 0.15f, 1.0f ) );
            if ( ImGui::Button( "Delete", ImVec2( 110.0f, 0.0f ) ) )
            {
                for ( const auto& path : m_PendingDeleteList )
                {
                    std::string err;
                    if ( !AssetFileOps::Delete( path, err ) )
                        m_On.OnStatus( "Delete failed: " + err );
                }
                m_Selection.Deselect();
                m_On.OnRefresh();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    Common::BoolResultStr AssetContextMenu::Rename()
    {
        if ( m_Selection.Current() == nullptr )
            return Common::MakeError( "rename: no asset is selected in the Assets window" );
        m_RenamePath = m_Selection.Current()->AssetPath;
        std::snprintf( m_RenameBuf, sizeof( m_RenameBuf ), "%s",
                       std::filesystem::path( m_RenamePath ).filename().string().c_str() );
        // What the dialog lists before the user confirms: the registry's edges, not a text scan.
        m_RenameReferrers = Assets::ContentRegistry::Referrers( m_RenamePath );
        m_ShowRenamePopup = true;
        return Common::MakeSuccess( true );
    }

    void AssetContextMenu::Paste( const DirectoryInformation* folder )
    {
        const std::vector<std::string>& clipboard = m_Selection.Clipboard();
        if ( clipboard.empty() || !folder )
            return;
        const bool cut = m_Selection.ClipboardIsCut();
        for ( const auto& src : clipboard )
        {
            std::string np, err;
            const bool  ok = cut ? ContentBrowserUtils::MoveOrRename( src,
                                                                      std::filesystem::path( folder->AssetPath ) /
                                                                           std::filesystem::path( src ).filename(),
                                                                      "Move", err )
                                 : AssetFileOps::CopyInto( src, folder->AssetPath, np, err );
            if ( !ok )
                m_On.OnStatus( "Paste failed: " + err );
        }
        m_Selection.Pasted(); // a cut is consumed by the paste
        m_On.OnRefresh();
    }

    void AssetContextMenu::CommandMenuItem( ContentBrowserCommand command, const DirectoryInformation* folder,
                                            bool selected, bool enabled )
    {
        const UICommandInfo& info     = CommandInfo( command );
        const std::string    label    = std::string( info.Label );
        const std::string    shortcut = std::string( info.Shortcut );
        if ( !ImGui::MenuItem( label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), selected, enabled ) )
            return;
        if ( const auto done = Run( command, folder ); !done )
            LOG_ERROR( "[Content Browser] {}: {}", info.Label, done.GetError() );
    }

    Common::BoolResultStr AssetContextMenu::Run( ContentBrowserCommand       command,
                                                 const DirectoryInformation* folder )
    {
        const std::string_view label = CommandInfo( command ).Label;
        if ( command == ContentBrowserCommand::ClearSelection )
        {
            m_Selection.Clear();
            return Common::MakeSuccess( true );
        }

        const std::vector<DirectoryInformation*> entries = m_Selection.EntriesIn( folder );
        if ( entries.empty() )
            return Common::MakeFormattedError<bool>( "'{}': nothing is selected in the Content Browser", label );
        const auto one = [&]() -> Common::ResultStr<DirectoryInformation*>
        {
            if ( entries.size() != 1 )
                return Common::MakeFormattedError<DirectoryInformation*>(
                     "'{}' acts on one asset, and {} are selected", label, entries.size() );
            return Common::MakeSuccess( entries.front() );
        };
        // One body over every selected entry; the first refusal is the command's answer, the others still run.
        const auto overEntries = [&]( const std::function<Common::BoolResultStr( DirectoryInformation& )>& body )
             -> Common::BoolResultStr
        {
            Common::BoolResultStr outcome = Common::MakeSuccess( true );
            for ( DirectoryInformation* entry : entries )
                if ( const auto done = body( *entry ); !done && outcome )
                    outcome = Common::MakeFormattedError<bool>( "'{}': {}", entry->AssetPath, done.GetError() );
            return outcome;
        };

        switch ( command )
        {
            case ContentBrowserCommand::Open:
            {
                if ( entries.size() == 1 && !entries.front()->IsFile )
                {
                    m_On.OnOpenFolder( entries.front() );
                    return Common::MakeSuccess( true );
                }
                return overEntries(
                     [&]( DirectoryInformation& entry ) -> Common::BoolResultStr
                     {
                         if ( !entry.IsFile )
                             return Common::MakeError<bool>(
                                  "a folder opens on its own, not in a multi-selection" );
                         ContentBrowserUtils::ShellOpenDefault( entry.AssetPath );
                         return Common::MakeSuccess( true );
                     } );
            }
            case ContentBrowserCommand::ShowInExplorer:
                return overEntries(
                     []( DirectoryInformation& entry ) -> Common::BoolResultStr
                     {
                         ContentBrowserUtils::ShellRevealInExplorer( entry.AssetPath );
                         return Common::MakeSuccess( true );
                     } );
            case ContentBrowserCommand::OpenContainingFolder:
                // The selection lives in one folder: one window, not one per entry.
                ContentBrowserUtils::ShellOpenDefault(
                     std::filesystem::path( entries.front()->AssetPath ).parent_path().string() );
                return Common::MakeSuccess( true );
            case ContentBrowserCommand::Reimport:
                return overEntries( []( DirectoryInformation& entry )
                                    { return ImportOptions::Reimport( entry.AssetPath ); } );
            case ContentBrowserCommand::ReimportWithNewFile:
            {
                const auto target = one();
                if ( !target )
                    return Common::MakeError<bool>( target.GetError() );
                const auto chosen =
                     DesktopPlatform::OpenFileDialog( "Meshes\0*.fbx;*.glb;*.gltf;*.obj\0All\0*.*\0" );
                if ( chosen.empty() )
                    return Common::MakeFormattedError<bool>( "'{}': no file was chosen", label );
                return ImportOptions::ReimportWithNewFile( target.GetValue()->AssetPath, chosen );
            }
            case ContentBrowserCommand::CaptureThumbnail:
                if ( !m_ThumbnailEdit.CanCapture() )
                    return Common::MakeFormattedError<bool>( "'{}': there is no viewport scene to capture",
                                                             label );
                return overEntries( [&]( DirectoryInformation& entry ) -> Common::BoolResultStr
                                    { return m_ThumbnailEdit.Capture( entry ); } );
            case ContentBrowserCommand::EditThumbnail:
            {
                // Edit Thumbnail asks the kind table (ThumbnailProducers::HasThumbnailOrbit, in Enter), not a
                // model-or-material list.
                const auto target = one();
                if ( !target )
                    return Common::MakeError<bool>( target.GetError() );
                return m_ThumbnailEdit.Enter( *target.GetValue(), label );
            }
            case ContentBrowserCommand::ClearSelection:
                break; // answered above: it needs no selection
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor

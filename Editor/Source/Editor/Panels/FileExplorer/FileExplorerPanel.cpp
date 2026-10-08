#define IMGUI_DEFINE_MATH_OPERATORS

// This TU now pulls engine headers (via UIHelper -> Engine/Desert.hpp) that use std::max/std::min and
// std::numeric_limits<>::max(); keep the windows.h min/max macros from clobbering them.
#define NOMINMAX

#include "FileExplorerPanel.hpp"
#include <Editor/Platform/DesktopPlatform.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserDragDrop.hpp>
#include <Editor/Core/SceneOpenRequest.hpp>
#include <Editor/Panels/Clouds/CloudDocumentOpen.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserImport.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserUtils.hpp>
#include <Editor/Panels/FileExplorer/FileTypeInfo.hpp>
#include <Editor/Panels/FileExplorer/NewCloudAsset.hpp>
#include <Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp>
#include <Editor/Core/AssetFileOps.hpp>
#include <Editor/Core/ContentCreateCommands.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Editor/Core/MaterialAssetUtils.hpp>
#include <Editor/Core/Commands/AssetMoveCommand.hpp>
#include <Editor/Core/AssetReferences.hpp>
#include <Editor/Core/EditorPreferences.hpp> // the pinned folders live in editor.json (К5)
#include <Editor/Panels/NodeGraph/NodeGraphPanel.hpp>
#include <Editor/Panels/NodeGraph/ShaderGraphDocumentOpen.hpp>
#include "../../Core/EditorResources.hpp"

#include <Editor/Import/ImportOptionsDialog.hpp>
#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Editor/Import/MeshMaterial.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Editor/Panels/FileExplorer/AssetTooltipLayout.hpp>
#include <cstdio>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailFoliage.hpp>
#include <Editor/Widgets/ThumbnailPose.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Editor/Widgets/ThumbnailPrefetch.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Engine/Core/Scene.hpp>       // GetFinalImage (Capture Thumbnail from viewport)
#include <Engine/Graphic/Renderer.hpp> // WaitDeviceIdle before readback
#include <Engine/Graphic/Image.hpp>    // Image2D::ReadPixelsRGBA8
#include <Common/Core/Events/WindowEvents.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

// STB_IMAGE_WRITE_IMPLEMENTATION lives in Desert.lib; just declare for the capture PNG write.
#include <stb_image/stb_image_write.h>

#include <ImGui/imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <Editor/Widgets/ThumbnailEdit.hpp>

#include <filesystem>
#include <utility>
#include <format>
#include <fstream>
#include <system_error>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    FileExplorerPanel::FileExplorerPanel( const std::filesystem::path& rootPath,
                                          const SubjectEditorRegistry* subjectEditors,
                                          AssetThumbnailPool& thumbnailPool, Assets::AssetManager* assetManager,
                                          std::weak_ptr<::Desert::Core::Scene> viewportScene )
         // IN DECLARATION ORDER. Members are constructed in the order they are DECLARED whatever this list
         // says, so a list in a different order is a reader being told the wrong sequence.
         : IPanel( "Assets" ),
           m_PathView( { .OnFolderSelected    = [this]( DirectoryInformation* dir ) { ChangeDirectory( dir ); },
                         .OnFavouriteSelected = [this]( const std::string& path ) { NavigateToPath( path ); },
                         .SelectedPaths       = [this] { return m_Selection.Paths(); },
                         .OnMoved             = [this] { QueueRefresh(); } } ),
           m_Toolbar( { .OnFolderSelected    = [this]( DirectoryInformation* dir ) { ChangeDirectory( dir ); },
                        .OnFavouriteSelected = [this]( const std::string& path ) { NavigateToPath( path ); },
                        .OnBack              = [this] { GoBack(); },
                        .OnForward           = [this] { GoForward(); },
                        .OnRefresh           = [this] { QueueRefresh(); },
                        .OnNewFolder =
                             [this]
                        {
                            if ( m_CurrentDir )
                                m_NewAssetMenu.CreateNewFolder( *m_CurrentDir );
                        },
                        .OnImport =
                             [this]
                        {
                            if ( ContentBrowserImport::ImportTextureFromDialog( m_AssetManager, m_CurrentDir ) )
                                QueueRefresh();
                        } } ),
           m_AssetView( { .OnDrawTile = [this]( int dirIndex, bool folder, int shownIndex, bool gridView )
                          { return RenderFile( dirIndex, folder, shownIndex, gridView ); },
                          .IsSelected = [this]( const DirectoryInformation* entry )
                          { return m_Selection.Contains( entry ); },
                          .OnBackgroundContextMenu = [this] { DrawBackgroundContextMenu(); },
                          .OnClearSelection        = [this] { m_Selection.Deselect(); } } ),
           m_Model( rootPath.string() ), m_AssetManager( assetManager ), m_SubjectEditors( subjectEditors ),
           m_ThumbnailPool( thumbnailPool ), m_TileThumbnail( m_ThumbnailPool, assetManager ),
           m_ViewportScene( std::move( viewportScene ) ),
           m_ThumbnailEdit(
                m_AssetManager, m_ViewportScene,
                { .CookedPictureOf = [this]( const std::string& path, FileType type ) -> std::optional<std::string>
                  {
                      if ( const auto picture = m_ThumbnailPool.MeshPictureFor( path, type ) )
                          return picture->Cooked;
                      return std::nullopt;
                  },
                  .MeshSourceOf = [this]( const DirectoryInformation& entry )
                  { return m_ThumbnailPool.MeshSourceFor( entry.AssetPath, entry.Type ); },
                  .Thumbnails  = [this]() -> ThumbnailCache& { return m_ThumbnailPool.Thumbnails(); },
                  .TextureIdOf = [this]( const std::shared_ptr<Graphic::Image2D>& image )
                  { return m_TileThumbnail.TextureIdOf( image ); },
                  .OnCaptured = [this]( const std::string& png, const std::string& assetPath )
                  { m_ThumbnailPool.OnCaptured( png, assetPath ); } } ),
           m_NewAssetMenu(
                m_AssetManager, m_ViewportScene,
                { .OnRefresh            = [this] { QueueRefresh(); },
                  .OnSelectAfterRefresh = [this]( const std::string& path ) { m_SelectAfterRefresh = path; },
                  .OnStatus             = [this]( const std::string& line ) { m_FileOpStatus = line; } } ),
           m_ItemMenu( m_Selection, m_ThumbnailEdit,
                       { .OnRefresh     = [this] { QueueRefresh(); },
                         .OnOpenFolder  = [this]( DirectoryInformation* dir ) { ChangeDirectory( dir ); },
                         .OnStatus      = [this]( const std::string& line ) { m_FileOpStatus = line; },
                         .CanAddToScene = [this] { return m_NewAssetMenu.CanAddPrefabToScene(); },
                         .OnAddToScene  = [this]( const std::string& path )
                         { m_NewAssetMenu.AddPrefabToScene( path ); } } )
    {
        // NO LOAD STEP FOR THE PINNED FOLDERS ANY MORE (К5). They are read out of EditorPreferences where
        // they are drawn; a panel-local copy taken at construction is the shape К6 removed from the gizmo
        // snap, and here it would additionally be a copy of a list the user can change while the panel is
        // alive (opening a different project changes which pins exist).

        if ( DirectoryInformation* root = m_Model.Root() )
        {
            m_CurrentDir = root;

            // REOPEN WHERE THE USER LEFT IT (THUMB3), as UE's browser does — and first, so the THUMB2
            // prefetch this navigation starts decodes the folder that will actually be on screen. A folder
            // that no longer exists is not an error: the browser opens at the root, as it always did.
            const std::optional<std::string> remembered = EditorPreferences::CurrentBrowserFolder();
            if ( !( remembered && NavigateToPath( *remembered ) ) )
                ChangeDirectory( root );
        }
        m_RestoringFolder = false;
    }

    // The cloud bake's worker is NewAssetMenu's, and its destructor cancels and waits for it.
    FileExplorerPanel::~FileExplorerPanel() = default;

    void FileExplorerPanel::OnPreUpdate()
    {
        // BEFORE the throttle and before the early return on a null directory: a generation that has
        // finished must be collected on the frame it finished, whatever the browser is looking at. A
        // future nobody polls is a thread whose result is thrown away at shutdown.
        m_NewAssetMenu.Poll();

        if ( m_CurrentDir != nullptr && m_Watcher.Poll( m_CurrentDir->AssetPath ) )
        {
            LOG_DEBUG( "[FileExplorer] '{}' changed on disk — rescanning.", m_CurrentDir->AssetPath );
            QueueRefresh();
        }
    }

    void FileExplorerPanel::ChangeDirectory( DirectoryInformation* directory )
    {
        if ( !directory )
            return;
        m_ThumbnailEdit.Leave(); // the edited tile is not in the folder being opened

        m_CurrentDir = directory;
        m_Toolbar.InvalidateBreadcrumbs();
        m_Model.Open( m_CurrentDir );

        PrefetchCurrentFolderThumbnails();
        if ( !m_RestoringFolder )
            EditorPreferences::RememberBrowserFolder( m_CurrentDir->AssetPath );

        m_Watcher.Rebase( m_CurrentDir->AssetPath );
        m_History.Record( directory->AssetPath ); // a back/forward step is not recorded (ContentBrowserHistory)
    }

    void FileExplorerPanel::PrefetchCurrentFolderThumbnails()
    {
        if ( m_CurrentDir != nullptr )
            m_ThumbnailPool.PrefetchFolder( *m_CurrentDir );
    }

    bool FileExplorerPanel::NavigateToPath( const std::string& path )
    {
        if ( DirectoryInformation* listed = m_Model.Find( path ) )
        {
            ChangeDirectory( listed );
            return true;
        }

        // Not loaded yet (e.g. a favorite from a previous session): expand the tree from the project
        // root down to `path`, matching one segment at a time.
        DirectoryInformation* cur = m_Model.Root();
        if ( !cur )
            return false;
        const std::filesystem::path base = cur->AssetPath;
        std::error_code             ec;
        const std::filesystem::path rel = std::filesystem::relative( path, base, ec );
        // "Not under the project" = the relative path starts with a ".." component. Compare path
        // ELEMENTS rather than the native string: path::native() is std::wstring on Windows, so a
        // narrow ".." literal does not even overload-resolve there.
        if ( ec || rel.empty() || *rel.begin() == std::filesystem::path( ".." ) )
            return false; // not under the project

        m_Model.Open( cur );
        std::filesystem::path acc = base;
        for ( const auto& seg : rel )
        {
            acc /= seg;
            DirectoryInformation* next = nullptr;
            for ( auto* ch : cur->Children )
                if ( !ch->IsFile && std::filesystem::path( ch->AssetPath ) == acc )
                {
                    next = ch;
                    break;
                }
            if ( !next )
                return false; // path no longer exists
            m_Model.Open( next );
            cur = next;
        }
        ChangeDirectory( cur );
        return true;
    }

    void FileExplorerPanel::GoBack()
    {
        m_History.Step( -1, [this]( const std::string& path ) { (void)NavigateToPath( path ); } );
    }

    void FileExplorerPanel::GoForward()
    {
        m_History.Step( +1, [this]( const std::string& path ) { (void)NavigateToPath( path ); } );
    }

    Common::BoolResultStr FileExplorerPanel::CreateNewLevelSequence()
    {
        return m_NewAssetMenu.CreateNewLevelSequence( m_CurrentDir );
    }

    ImVec2 GetAspectCorrectedSize( const ImVec2& originalSize, float maxSize )
    {
        float aspect = originalSize.x / originalSize.y;
        if ( aspect > 1.0f )
            return { maxSize, maxSize / aspect }; // Wider than tall
        else
            return { maxSize * aspect, maxSize }; // Taller than wide or square
    }

    void FileExplorerPanel::OnUIRender()
    {
        m_ItemMenu.HandleShortcuts( m_CurrentDir );

        // File-op modals (rename / delete) + last error line.
        m_ItemMenu.DrawPopups();
        DrawFileOpStatus();
        m_NewAssetMenu.DrawBakeStatus();
        ApplyPendingRefresh();

        // ── Content Browser (UE SContentBrowser): two panes split by a draggable vertical splitter. LEFT = the
        //    path view (pinned Favorites + the project folder tree); RIGHT = the toolbar with its breadcrumbs,
        //    then the asset view. ──
        constexpr float kMinTreeWidth    = 120.0f;
        constexpr float kMinContentWidth = 220.0f;
        constexpr float kSplitterW       = 6.0f;
        const float     totalAvail       = ImGui::GetContentRegionAvail().x;
        m_TreeWidth                      = std::clamp( m_TreeWidth, kMinTreeWidth,
                                                       std::max( kMinTreeWidth, totalAvail - kMinContentWidth - kSplitterW ) );

        m_PathView.Draw( m_Model, m_CurrentDir, m_TreeWidth );

        // SPLITTER — a thin invisible handle the user drags to resize the tree pane.
        ImGui::SameLine( 0.0f, 0.0f );
        // GetContentRegionAvail().y can be 0 on a first/zero-height frame; InvisibleButton asserts on a
        // zero size, so floor the height at 1px (harmless — the handle is invisible anyway).
        const float cbSplitterH = ImGui::GetContentRegionAvail().y;
        ImGui::InvisibleButton( "##cb_splitter", ImVec2( kSplitterW, cbSplitterH > 0.0f ? cbSplitterH : 1.0f ) );
        if ( ImGui::IsItemActive() )
            m_TreeWidth += ImGui::GetIO().MouseDelta.x;
        if ( ImGui::IsItemHovered() || ImGui::IsItemActive() )
            ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeEW );
        {
            const ImVec2 mn  = ImGui::GetItemRectMin();
            const ImVec2 mx  = ImGui::GetItemRectMax();
            const bool   hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            ImGui::GetWindowDrawList()->AddRectFilled(
                 ImVec2( ( mn.x + mx.x ) * 0.5f - 1.0f, mn.y ), ImVec2( ( mn.x + mx.x ) * 0.5f + 1.0f, mx.y ),
                 ImGui::GetColorU32( hot ? ImGuiCol_SeparatorActive : ImGuiCol_Separator ) );
        }
        ImGui::SameLine( 0.0f, 0.0f );

        // RIGHT PANE.
        ImGui::BeginChild( "##cb_right", ImVec2( 0.0f, 0.0f ), false );
        m_Toolbar.Draw( m_ViewState, m_History, m_CurrentDir, m_Model.Root() );
        if ( m_CurrentDir )
            m_AssetView.Draw( *m_CurrentDir, m_ViewState, m_Model.ShowsHiddenFiles() );
        ImGui::EndChild(); // ##cb_right
    }

    void FileExplorerPanel::DrawFileOpStatus()
    {
        if ( m_FileOpStatus.empty() )
            return;
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", m_FileOpStatus.c_str() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "x##clearFileOp" ) )
            m_FileOpStatus.clear();
    }

    void FileExplorerPanel::ApplyPendingRefresh()
    {
        if ( m_Refresh )
        {
            RefreshCurrentDirectory(); // in-place: keeps navigation (watcher / import / rebuild)
            m_Refresh = false;
            if ( !m_SelectAfterRefresh.empty() )
            {
                if ( const auto selected = SelectEntry( std::exchange( m_SelectAfterRefresh, {} ) ); !selected )
                    LOG_ERROR( "[Content] the new asset was created but not selected: {}", selected.GetError() );
            }
        }
    }

    void FileExplorerPanel::DrawBackgroundContextMenu()
    {
        const auto& clipboard = m_Selection.Clipboard();
        if ( !clipboard.empty() &&
             ImGui::Selectable( m_Selection.ClipboardIsCut() ? "Paste (move)" : "Paste (copy)" ) )
            m_ItemMenu.Paste( m_CurrentDir );
        m_ItemMenu.DrawTrashMenu();

        ImGui::Separator();

        if ( ImGui::Selectable( "Import Texture..." ) )
            if ( ContentBrowserImport::ImportTextureFromDialog( m_AssetManager, m_CurrentDir ) )
                QueueRefresh();

        if ( ImGui::Selectable( "Refresh" ) )
            QueueRefresh();

        if ( m_CurrentDir )
            m_NewAssetMenu.Draw( *m_CurrentDir );
    }

    // Emit the drag-drop payloads a dragged asset can be dropped as. Target widgets accept exactly the
    // type they expect (texture slots: TEXTURE_ASSET; material slots: MATERIAL_ASSET; hierarchy: PREFAB_FILE).
    void FileExplorerPanel::EmitAssetDragSource( const DirectoryInformation& entry )
    {
        if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceAllowNullID ) )
        {
            const std::string& assetPath = entry.AssetPath;
            const char*        type      = ContentBrowserDragDrop::PayloadTypeOf( entry.IsFile, entry.Type );

            ImGui::SetDragDropPayload( type, assetPath.c_str(), assetPath.size() + 1 );

            m_TileThumbnail.DrawDragPreview( entry );
            ImGui::EndDragDropSource();
        }
    }

    bool FileExplorerPanel::OnWindowFileDropped( Common::EventWindowFileDrop& drop )
    {
        bool landed = false;
        for ( const auto& path : drop.Paths )
            landed = ContentBrowserImport::ImportFile( m_AssetManager, path, m_CurrentDir ) || landed;
        if ( landed )
            QueueRefresh();
        return true;
    }

    std::vector<std::string> FileExplorerPanel::SelectionPaths() const
    {
        return m_Selection.Paths();
    }

    std::vector<std::string> FileExplorerPanel::ShownEntries( bool folders ) const
    {
        std::vector<std::string> shown;
        if ( m_CurrentDir != nullptr )
            for ( const DirectoryInformation* child : m_CurrentDir->Children )
                if ( !child->Hidden && child->IsFile != folders )
                    shown.push_back( child->AssetPath );
        return shown;
    }

    Common::BoolResultStr FileExplorerPanel::SelectEntry( const std::string& path )
    {
        if ( m_CurrentDir != nullptr )
            for ( DirectoryInformation* child : m_CurrentDir->Children )
                if ( child->AssetPath == path )
                {
                    m_Selection.SelectOnly( *child );
                    return Common::MakeSuccess( true );
                }
        return Common::MakeError( "select: '" + path + "' is not shown in the Assets window's current folder" );
    }

    Common::BoolResultStr FileExplorerPanel::RenameSelected()
    {
        return m_ItemMenu.Rename();
    }

    Common::BoolResultStr FileExplorerPanel::RunCommand( ContentBrowserCommand command )
    {
        return m_ItemMenu.Run( command, m_CurrentDir );
    }

    std::vector<FileExplorerPanel::ThumbnailOrbitSubject> FileExplorerPanel::SelectedThumbnailSubjects()
    {
        return m_ThumbnailEdit.SubjectsOf( m_Selection.EntriesIn( m_CurrentDir ) );
    }

    std::vector<std::string> FileExplorerPanel::ContentFolders() const
    {
        return m_Model.AllFolders();
    }

    std::vector<std::string> FileExplorerPanel::ContentFiles() const
    {
        return m_Model.AllFiles();
    }

    Common::BoolResultStr FileExplorerPanel::GoToFolder( const std::string& path )
    {
        if ( !NavigateToPath( path ) )
            return Common::MakeFormattedError<bool>( "{}: '{}' is not a folder under the Content Browser's root",
                                                     kGoToFolderLabel, path );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr FileExplorerPanel::SyncToAsset( const std::string& path )
    {
        const std::string folder = std::filesystem::path( path ).parent_path().generic_string();
        if ( !NavigateToPath( folder ) )
            return Common::MakeFormattedError<bool>(
                 "{}: the folder of '{}' is not under the Content Browser's root", kSyncToAssetLabel, path );
        if ( auto selected = SelectEntry( path ); !selected )
            return Common::MakeFormattedError<bool>( "{}: '{}' is not a file in '{}'", kSyncToAssetLabel, path,
                                                     folder );
        return Common::MakeSuccess( true );
    }

    ContentBrowserAssetView::TileResult FileExplorerPanel::RenderFile( int dirIndex, bool folder, int shownIndex,
                                                                       bool gridView )
    {
        DirectoryInformation* entry         = m_CurrentDir->Children[dirIndex];
        bool                  doubleClicked = false;
        bool                  hovered       = false;

        const std::string fileName = std::filesystem::path( entry->AssetPath ).filename().string();
        const char*       icon     = folder ? ICON_MDI_FOLDER : FileTypeInfoOf( entry->Type ).Icon;

        ImGui::PushID( dirIndex );

        if ( gridView )
        {
            const float thumb  = m_ViewState.GridSize * 0.66f;
            const float cellW  = m_ViewState.GridSize;
            const float indent = ( cellW - thumb ) * 0.5f; // center the icon/thumbnail in the cell

            // Content is emitted on the TOP draw-list channel; the card + thumbnail tile go on the BOTTOM
            // one behind it (channel split).
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::BeginGroup();
            if ( indent > 0.0f )
                ImGui::Indent( indent );

            // Texture/material/model -> live thumbnail; everything else -> a big coloured type icon. The
            // thumbnail/icon IS the hoverable/selectable/draggable item.
            const bool drewThumb = entry->IsFile && m_TileThumbnail.DrawThumbnail( entry, ImVec2( thumb, thumb ) );
            if ( !drewThumb )
            {
                const ImVec4 col = entry->IsFile ? entry->FileTypeColour : ImVec4( 0.95f, 0.82f, 0.42f, 1.0f );
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
                ImGui::PushStyleColor( ImGuiCol_Text, col );
                ImGui::PushFont( EditorResources::GetBigIconFont() );
                ImGui::Button( icon, ImVec2( thumb, thumb ) );
                ImGui::PopFont();
                ImGui::PopStyleColor( 4 );
            }

            const ImVec2 thumbMin = ImGui::GetItemRectMin();
            const ImVec2 thumbMax = ImGui::GetItemRectMax();

            // In Edit Thumbnail mode the tile's drag is the orbit, not an asset drag.
            const bool editingThumbnail = entry->IsFile && m_ThumbnailEdit.IsEditing( entry->AssetPath );
            if ( editingThumbnail )
                m_ThumbnailEdit.Draw( *entry, thumbMin, thumbMax );

            if ( ImGui::IsItemClicked() )
                m_Selection.Click( *entry, shownIndex, *m_CurrentDir, m_ViewState, m_Model.ShowsHiddenFiles() );
            if ( ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                doubleClicked = true;

            if ( !editingThumbnail )
                EmitAssetDragSource( *entry );
            if ( folder )
                m_PathView.AcceptMoveDropOnLastItem( entry->AssetPath ); // a folder tile takes a move-drop too
            m_ItemMenu.Draw( *entry, m_CurrentDir );

            if ( ImGui::IsItemHovered() && !ImGui::IsDragDropActive() )
                m_TileThumbnail.DrawTooltip( &*entry );
            else
                m_TileThumbnail.ForgetTooltip( &*entry );

            // Type badge — a small coloured pill (the extension) at the tile's bottom-right, files only.
            if ( entry->IsFile )
            {
                std::string ext = std::filesystem::path( entry->AssetPath ).extension().string();
                if ( !ext.empty() && ext.front() == '.' )
                    ext.erase( ext.begin() );
                std::transform( ext.begin(), ext.end(), ext.begin(),
                                []( unsigned char c ) { return static_cast<char>( std::toupper( c ) ); } );
                if ( ext.size() > 4 )
                    ext.resize( 4 );
                if ( !ext.empty() )
                {
                    const ImVec4 c  = entry->FileTypeColour;
                    const ImVec2 ts = ImGui::CalcTextSize( ext.c_str() );
                    const ImVec2 bpad( 4.0f, 1.0f );
                    const ImVec2 bmax( thumbMax.x - 2.0f, thumbMax.y - 2.0f );
                    const ImVec2 bmin( bmax.x - ts.x - bpad.x * 2.0f, bmax.y - ts.y - bpad.y * 2.0f );
                    dl->AddRectFilled( bmin, bmax,
                                       IM_COL32( (int)( c.x * 255 ), (int)( c.y * 255 ), (int)( c.z * 255 ), 235 ),
                                       3.0f );
                    dl->AddText( ImVec2( bmin.x + bpad.x, bmin.y + bpad.y ), IM_COL32( 15, 15, 18, 255 ),
                                 ext.c_str() );
                }
            }

            if ( indent > 0.0f )
                ImGui::Unindent( indent );

            // Label: single line, centered under the tile, ellipsized to the cell width.
            {
                std::string shown = fileName;
                if ( ImGui::CalcTextSize( shown.c_str() ).x > cellW )
                {
                    while ( shown.size() > 1 && ImGui::CalcTextSize( ( shown + "..." ).c_str() ).x > cellW )
                        shown.pop_back();
                    shown += "...";
                }
                const float tw = ImGui::CalcTextSize( shown.c_str() ).x;
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + std::max( 0.0f, ( cellW - tw ) * 0.5f ) );
                ImGui::TextUnformatted( shown.c_str() );
            }

            ImGui::EndGroup();

            // Card + thumbnail tile behind the content: rounded, subtle by default, brighter on hover,
            // filled + accent-ringed when selected. Card spans the full cell width (centered on the thumb).
            const float  cellLeft = thumbMin.x - indent;
            const ImVec2 cmin( cellLeft - 2.0f, thumbMin.y - 8.0f );
            const ImVec2 cmax( cellLeft + cellW + 2.0f, ImGui::GetItemRectMax().y + 6.0f );
            const bool   sel   = m_Selection.Contains( entry );
            const bool   hover = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect( cmin, cmax );
            if ( hover )
                hovered = true; // consumed by the asset view's empty-click deselect
            ImU32 bg = IM_COL32( 255, 255, 255, 10 );
            if ( sel )
                bg = IM_COL32( 52, 92, 160, 150 );
            else if ( hover )
                bg = IM_COL32( 255, 255, 255, 24 );
            dl->ChannelsSetCurrent( 0 );
            dl->AddRectFilled( cmin, cmax, bg, 8.0f );
            if ( sel )
                dl->AddRect( cmin, cmax, IM_COL32( 120, 170, 255, 255 ), 8.0f, 0, 1.5f );
            // Rounded tile behind the thumbnail/icon.
            dl->AddRectFilled( ImVec2( thumbMin.x - 5.0f, thumbMin.y - 5.0f ),
                               ImVec2( thumbMax.x + 5.0f, thumbMax.y + 5.0f ), IM_COL32( 0, 0, 0, 60 ), 6.0f );
            dl->ChannelsMerge();
        }
        else
        {
            const std::string label = std::string( icon ) + "  " + fileName;
            if ( ImGui::Selectable( label.c_str(), m_Selection.Contains( entry ),
                                    ImGuiSelectableFlags_AllowDoubleClick ) )
            {
                m_Selection.Click( *entry, shownIndex, *m_CurrentDir, m_ViewState, m_Model.ShowsHiddenFiles() );
                if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                    doubleClicked = true;
            }
            if ( ImGui::IsItemHovered() )
                hovered = true;
            EmitAssetDragSource( *entry );
            if ( folder )
                m_PathView.AcceptMoveDropOnLastItem( entry->AssetPath );
            m_ItemMenu.Draw( *entry, m_CurrentDir );
        }

        if ( doubleClicked && folder )
        {
            ChangeDirectory( entry );
        }
        // ── THE ONE FILE KIND THAT IS NOT A DOCUMENT ──────────────────────────────────────────────────
        //
        // A SCENE is not a document: it is what every other window is about, and opening one replaces the
        // world rather than adding a window to it — so it goes through its own guarded load, the same one
        // a drop and the File menu use.
        //
        // THE SHADER GRAPH USED TO BE THE SECOND ARM HERE, and the note that stood with it listed four
        // things U7-2 had measured as being in the way of making it a document. Three were "there is no
        // `AssetTypeID::ShaderGraph`", and they went with the type. The fourth — New/Load/double-click
        // replacing the open graph with no prompt — stopped being expressible when one window became one
        // graph. The one that looked most expensive was FALSE: a `.dgraph` needed no id of its own,
        // because `AssetHandle` is derived from the project-relative path (see the note on
        // Engine/Assets/Serialization/ShaderGraph.hpp's Document). Zero `.dgraph` files were migrated.
        else if ( doubleClicked && entry->Type == FileType::Scene )
        {
            Core::SceneOpenRequest::Request( entry->AssetPath );
        }
        // ── EVERYTHING ELSE: ASK THE REGISTRY ─────────────────────────────────────────────────────────
        //
        // One question, whatever the format. This used to be a chain of `else if` — one arm for `.demat`,
        // one for the four cloud extensions — each of which had to know which opener to call and what its
        // three outcomes meant, and EditorLayer carried a second copy of the same chain for its own
        // path-to-document resolution. A new kind of document was an edit here, an edit there, and a
        // registration; the two that were not the registration are the ones that got forgotten.
        //
        // NotMine is SILENT and that is correct: a `.png` is not a document, and most double-clicks in
        // this browser land on files nothing opens. Failed has already been logged BY THE OPENER, with the
        // path and the reason — reporting it again here would print two messages, the second of them
        // guessing.
        else if ( doubleClicked && m_SubjectEditors )
        {
            (void)m_SubjectEditors->OpenPath( entry->AssetPath );
        }

        ImGui::PopID();
        return { .DoubleClicked = doubleClicked, .Hovered = hovered };
    }

    void FileExplorerPanel::RefreshCurrentDirectory()
    {
        // The thumbnail cache is NOT cleared here: it checks each picture's file stamp on every Get, so a
        // rewritten picture is re-read anyway, and this runs on every rescan — including the one the
        // directory poll makes ~0.5 s after a folder is opened. Clearing here dropped every picture on screen
        // and put all of them back through the decoder at once (the Materials folder's 36-42 main-thread
        // decodes, TH2).
        if ( !m_CurrentDir )
            return;

        // The rescan frees the child DirectoryInformation entries, so any raw pointer into them (the
        // selection) would dangle. Remember it by its stable path and re-resolve after the rescan.
        const std::string selectedPath = m_Selection.ReleaseCurrent();

        // Re-scan the current directory in place — navigation (m_CurrentDir / the tree) is preserved.
        m_Model.Rescan( m_CurrentDir );
        m_Toolbar.InvalidateBreadcrumbs();

        if ( !selectedPath.empty() )
            m_Selection.Rebind( m_Model.Find( selectedPath ) );
    }

} // namespace Desert::Editor

static_assert( Common::HandlesEvent<Desert::Editor::FileExplorerPanel, Common::EventWindowFileDrop> );

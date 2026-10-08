#define IMGUI_DEFINE_MATH_OPERATORS

// This TU now pulls engine headers (via UIHelper -> Engine/Desert.hpp) that use std::max/std::min and
// std::numeric_limits<>::max(); keep the windows.h min/max macros from clobbering them.
#define NOMINMAX

#include "FileExplorerPanel.hpp"
#include <Editor/Platform/DesktopPlatform.hpp>
#include <Editor/Core/DragPayloads.hpp>
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

    FileExplorerPanel::FileExplorerPanel( const std::filesystem::path&         rootPath,
                                          const SubjectEditorRegistry*         subjectEditors,
                                          Assets::AssetManager*                assetManager,
                                          std::weak_ptr<::Desert::Core::Scene> viewportScene )
         // IN DECLARATION ORDER. Members are constructed in the order they are DECLARED whatever this list
         // says, so a list in a different order is a reader being told the wrong sequence.
         : IPanel( "Assets" ),
           m_PathView( { .OnFolderSelected    = [this]( DirectoryInformation* dir ) { ChangeDirectory( dir ); },
                         .OnFavouriteSelected = [this]( const std::string& path ) { NavigateToPath( path ); } } ),
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
           m_ViewportScene( std::move( viewportScene ) ),
           m_ThumbnailEdit(
                m_AssetManager, m_ViewportScene,
                { .CookedPictureOf = [this]( const std::string& path, FileType type ) -> std::optional<std::string>
                  {
                      if ( const std::optional<MeshPicture> picture = MeshPictureFor( path, type ) )
                          return picture->Cooked;
                      return std::nullopt;
                  },
                  .MeshSourceOf = [this]( const DirectoryInformation& entry ) { return MeshSourceFor( entry ); },
                  .TextureOf    = [this]( const std::string& png ) -> ImTextureID
                  {
                      if ( !m_Thumbnails || !m_UIHelper )
                          return nullptr;
                      const auto img = m_Thumbnails->Get( png );
                      return img ? m_UIHelper->GetTextureID( img ) : nullptr;
                  },
                  .OnCaptured =
                       [this]( const std::string& png, const std::string& assetPath )
                  {
                      if ( m_Thumbnails )
                          m_Thumbnails->Invalidate( png ); // drop the cached decode: the grid reloads
                      m_FailedThumbs.erase( assetPath );   // a refused tile shows the captured picture
                  } } ),
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
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_Thumbnails = std::make_unique<ThumbnailCache>();
        ThumbnailCache::PurgeOldVersions(); // drop stale-renderer thumbnails so they regenerate cleanly

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
        if ( m_CurrentDir == nullptr )
            return;

        // The same picture each Draw*Thumbnail below will pass to ThumbnailCache::Get, and the same
        // freshness subject it judges — a mismatch here would only cost a wasted decode (Get takes nothing
        // it was not asked for), never a wrong picture.
        std::vector<ThumbnailPrefetch::Item> items;
        for ( const DirectoryInformation* entry : m_CurrentDir->Children )
        {
            if ( entry == nullptr || !entry->IsFile || entry->Hidden )
                continue;
            using ThumbnailProducers::Producer;
            switch ( ThumbnailProducers::ProducerOf( entry->Type ).value_or( Producer::TypeIcon ) )
            {
                case Producer::Decoded:
                    items.push_back( { entry->AssetPath, {} } );
                    break;
                case Producer::RenderedMaterial:
                case Producer::RenderedPose: // a .skmesh is its own cooked form and freshness source
                case Producer::RenderedSky:  // keyed and judged on the skybox's own file
                case Producer::Painted:
                    items.push_back( { ThumbnailPngFor( entry->AssetPath ), entry->AssetPath } );
                    break;
                case Producer::RenderedMesh:
                {
                    const std::optional<MeshPicture> picture = MeshPictureFor( entry->AssetPath, entry->Type );
                    if ( !picture )
                        break;
                    items.push_back( { ThumbnailKey::DiskPath( picture->Cooked ), picture->Cooked } );
                    break;
                }
                case Producer::NotYetProduced:
                case Producer::TypeIcon:
                    break;
            }
        }
        m_PrefetchItems = items;
        // The project's pictures stay asked for: Request replaces what is waiting, and the folder must not
        // push them out of the queue (nor the other way round). What the cache already holds is not handed
        // over at all (THM1n-13): the splash made the project resident, so entering a folder decodes nothing.
        items.insert( items.end(), m_ProjectPrefetchItems.begin(), m_ProjectPrefetchItems.end() );
        ThumbnailPrefetch::Get().Request(
             ThumbnailPrefetch::Unresident( std::move( items ), [this]( const std::string& picture )
                                            { return m_Thumbnails->Holds( picture ); } ) );
    }

    const std::string& FileExplorerPanel::ThumbnailPngFor( const std::string& assetPath )
    {
        auto it = m_ThumbnailPngOf.find( assetPath );
        if ( it == m_ThumbnailPngOf.end() )
            it = m_ThumbnailPngOf.emplace( assetPath, ThumbnailKey::DiskPath( assetPath ) ).first;
        return it->second;
    }

    std::size_t FileExplorerPanel::WarmProjectThumbnails( const std::vector<ThumbnailWarmup::WarmItem>& scene,
                                                          const std::vector<ThumbnailWarmup::WarmItem>& project )
    {
        using ThumbnailWarmup::WarmItem;
        using ThumbnailWarmup::WarmKind;
        if ( m_AssetManager == nullptr )
            return 0;

        // A mesh is judged and filed by its COOKED form, as its tile does (DrawRenderedMeshThumbnail): the
        // picture is under the .stmesh and its freshness source is MeshFreshnessSource of it. A foliage type
        // is its mesh's picture (MeshSourceFor); a .skmesh is its own cooked form; a skinned source is the
        // pose of the asset its import wrote (MeshPictureFor), resolved as a Pose.
        const auto pictureOf = [this]( const WarmItem& item ) -> std::optional<MeshPicture>
        {
            if ( item.Kind == WarmKind::Pose )
                return MeshPicture{ item.Path, true };
            return MeshPictureFor( item.Path, ThumbnailWarmup::FileTypeOfPath( item.Path ) );
        };
        const auto cookedOf = [&pictureOf]( const WarmItem& item ) -> std::optional<std::string>
        {
            const std::optional<MeshPicture> picture = pictureOf( item );
            if ( !picture )
                return std::nullopt;
            return picture->Cooked;
        };
        const auto verdictOf = [&]( const WarmItem& item )
        {
            if ( item.Kind == WarmKind::Decoded )
                return ThumbnailFreshness::Verdict::Show; // the file is its own picture
            if ( item.Kind == WarmKind::Mesh || item.Kind == WarmKind::Pose )
            {
                const std::optional<std::string> cooked = cookedOf( item );
                if ( !cooked )
                    return ThumbnailFreshness::Verdict::Show; // refused and named by MeshSourceFor: no capture
                return ThumbnailService::JudgeMeshPicture( *cooked ); // the tile's and the enqueue gate's verdict
            }
            if ( item.Kind == WarmKind::Sky )
                return ThumbnailService::JudgeSkyboxPicture( item.Path ); // the tile's and RequestSkybox's verdict
            return ThumbnailFreshness::Judge(
                 ThumbnailFreshness::Observe( ThumbnailPngFor( item.Path ), item.Path ) );
        };
        const auto needsCapture = [&]( const WarmItem& item ) {
            return !m_FailedThumbs.contains( item.Path ) &&
                   verdictOf( item ) == ThumbnailFreshness::Verdict::Capture;
        };

        // EVERY PICTURE OF THE PROJECT IS ASKED FOR — the one the disk has goes to a worker decode now, the one
        // a capture below writes is asked for again when the captures have landed (RequestProjectPictures).
        m_ProjectPrefetchItems.clear();
        for ( const std::vector<WarmItem>* list : { &scene, &project } )
        {
            for ( const WarmItem& item : *list )
            {
                if ( item.Kind == WarmKind::Decoded )
                    m_ProjectPrefetchItems.push_back( { item.Path, {} } );
                else if ( item.Kind == WarmKind::Mesh || item.Kind == WarmKind::Pose )
                {
                    if ( const std::optional<std::string> cooked = cookedOf( item ) )
                        m_ProjectPrefetchItems.push_back( { ThumbnailKey::DiskPath( *cooked ), *cooked } );
                }
                else
                    m_ProjectPrefetchItems.push_back( { ThumbnailPngFor( item.Path ), item.Path } );
            }
        }

        const std::vector<WarmItem> warm = ThumbnailWarmup::SplashWarmList( scene, project, needsCapture );
        m_WarmMeshesPending.clear();
        for ( const WarmItem& item : warm )
        {
            if ( !needsCapture( item ) )
                continue; // a fresh scene subject: its picture is decoded with the rest
            switch ( item.Kind )
            {
                case WarmKind::Mesh:
                case WarmKind::Pose:
                {
                    // Resolved by TickWarmMeshes with the path each resolver takes, as the tile resolves it: a
                    // pose by its cooked asset (ResolvePoseSubject), a static mesh by the FILE ITS PICTURE IS OF
                    // (MeshFreshnessSource: a hand-authored .stmesh itself, else the raw source beside it).
                    // ResolveMesh asks the DDC for the source's import; handed the cooked path of an import
                    // (a scene root names base.stmesh, which an import never writes) it hashed a file that is
                    // not on disk and logged "Could not read file" for every imported mesh the scene used.
                    const std::optional<MeshPicture> picture = pictureOf( item );
                    if ( !picture )
                        break;
                    if ( picture->Pose )
                        m_WarmMeshesPending.push_back( { picture->Cooked, WarmKind::Pose } );
                    else
                        m_WarmMeshesPending.push_back(
                             { ThumbnailFreshness::MeshFreshnessSource( picture->Cooked ).generic_string(),
                               WarmKind::Mesh } );
                    break;
                }
                case WarmKind::Painted:
                    ThumbnailService::Get().WarmPainted( item.Path );
                    break;
                case WarmKind::Sky:
                {
                    // The row the registry filed it under names the handle, as the tile's request does.
                    const Assets::AssetHandle skybox = Runtime::SkyboxHandleAtPath( item.Path );
                    if ( static_cast<uint64_t>( skybox ) == 0 )
                    {
                        LOG_WARN( "[Thumbnails] the splash cannot warm '{}': the registry has no skybox row at it",
                                  item.Path );
                        m_FailedThumbs.insert( item.Path );
                        break;
                    }
                    ThumbnailService::Get().WarmSkybox( skybox, item.Path );
                    break;
                }
                case WarmKind::Material:
                {
                    // Resolved on a worker when it is not read yet; the arrival queues it as the tile's would.
                    const auto subject = ThumbnailSubject::ResolveMaterial(
                         *m_AssetManager, item.Path,
                         []( const std::string&                                   assetPath,
                             const Common::ResultStr<ThumbnailSubject::Material>& resolved )
                         {
                             if ( resolved )
                                 ThumbnailService::Get().WarmMaterial( resolved.GetValue(), assetPath );
                         } );
                    if ( !subject )
                    {
                        LOG_WARN( "[Thumbnails] the splash cannot warm '{}': {}", item.Path, subject.GetError() );
                        m_FailedThumbs.insert( item.Path );
                        break;
                    }
                    if ( const auto& material = subject.GetValue() )
                        ThumbnailService::Get().WarmMaterial( *material, item.Path );
                    break;
                }
                case WarmKind::Decoded:
                    break;
            }
        }
        (void)TickWarmMeshes();
        RequestProjectPictures();
        return ThumbnailService::Get().SceneWarmPending() + m_WarmMeshesPending.size();
    }

    void FileExplorerPanel::RequestProjectPictures()
    {
        std::vector<ThumbnailPrefetch::Item> items = m_PrefetchItems;
        items.insert( items.end(), m_ProjectPrefetchItems.begin(), m_ProjectPrefetchItems.end() );
        ThumbnailPrefetch::Get().Request(
             ThumbnailPrefetch::Unresident( std::move( items ), [this]( const std::string& picture )
                                            { return m_Thumbnails->Holds( picture ); } ) );
    }

    std::size_t FileExplorerPanel::ResidentThumbnails() const
    {
        return m_Thumbnails ? m_Thumbnails->ResidentCount() : 0;
    }

    std::size_t FileExplorerPanel::TickWarmMeshes()
    {
        if ( m_AssetManager == nullptr || m_WarmMeshesPending.empty() )
            return 0;
        std::erase_if( m_WarmMeshesPending,
                       [this]( const ThumbnailWarmup::WarmItem& item )
                       {
                           const bool         pose = item.Kind == ThumbnailWarmup::WarmKind::Pose;
                           const std::string& path = item.Path;
                           const auto subject = pose ? ThumbnailPose::ResolvePoseSubject( *m_AssetManager, path )
                                                     : ThumbnailSubject::ResolveMesh( *m_AssetManager, path );
                           if ( !subject )
                           {
                               // The same refusal the tile would log and blacklist; once, here, instead.
                               LOG_WARN( "[Thumbnails] the splash cannot warm '{}': {}", path,
                                         subject.GetError() );
                               m_FailedThumbs.insert( path );
                               return true;
                           }
                           if ( subject.GetValue().Pending )
                               return false; // read in flight: asked again next frame
                           if ( pose )
                               ThumbnailService::Get().WarmPose( subject.GetValue() );
                           else
                               ThumbnailService::Get().WarmMesh( subject.GetValue() );
                           return true;
                       } );
        return m_WarmMeshesPending.size();
    }

    std::size_t FileExplorerPanel::UploadPrefetchedThumbnails()
    {
        std::vector<ThumbnailPrefetch::Item> items = m_PrefetchItems;
        items.insert( items.end(), m_ProjectPrefetchItems.begin(), m_ProjectPrefetchItems.end() );
        const ThumbnailPrefetch::Survey survey = ThumbnailPrefetch::Get().SurveyOf( items );
        for ( const std::string& picture : survey.Ready )
            (void)m_Thumbnails->Get( picture );
        return survey.Pending;
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

        m_PathView.AcceptMoveDropOnLastItem();
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
            const char*        type      = "AssetFile";
            if ( entry.Type == FileType::Prefab )
                type = ::Desert::Editor::DragPayloads::PrefabFile;
            else if ( entry.Type == FileType::Texture )
                type = ::Desert::Editor::DragPayloads::TextureAsset;
            else if ( entry.Type == FileType::Material )
                type = ::Desert::Editor::DragPayloads::MaterialAsset;
            else if ( entry.Type == FileType::Model )
                type = ::Desert::Editor::DragPayloads::MeshAsset;
            else if ( entry.Type == FileType::Font )
                type = ::Desert::Editor::DragPayloads::FontFile;
            else if ( entry.Type == FileType::Scene )
                type = ::Desert::Editor::DragPayloads::SceneFile;

            ImGui::SetDragDropPayload( type, assetPath.c_str(), assetPath.size() + 1 );

            // Drag preview: the tile's thumbnail (texture/material/model, when cached) or the big
            // coloured type icon, with the filename beside it — mirrors what the user grabbed.
            std::shared_ptr<Graphic::Image2D> img;
            if ( m_Thumbnails )
            {
                if ( entry.Type == FileType::Texture )
                    img = m_Thumbnails->Get( assetPath );
                else if ( entry.Type == FileType::Material || entry.Type == FileType::Cloud )
                    img = m_Thumbnails->Get( ThumbnailKey::DiskPath( assetPath ) );
                else if ( entry.Type == FileType::Model )
                {
                    // THE COOKED KEY, not the source one. This branch used to share the material's line,
                    // so it looked up the picture under `DiskPath(<source>.fbx)` — a name nothing has
                    // written since M10 moved a mesh's thumbnail onto its cooked `.stmesh`. The ghost has
                    // been silently falling back to the type icon for every mesh ever since, which is
                    // exactly the kind of "it still works, just worse" a re-read site decays into.
                    if ( const std::optional<MeshPicture> picture = MeshPictureFor( assetPath, entry.Type ) )
                        img = m_Thumbnails->Get( ThumbnailKey::DiskPath( picture->Cooked ) );
                }
            }

            constexpr float previewSize = 48.0f;
            if ( img && m_UIHelper )
                m_UIHelper->Image( img, ImVec2( previewSize, previewSize ) );
            else
            {
                const char*  icon = entry.IsFile ? FileTypeInfoOf( entry.Type ).Icon : ICON_MDI_FOLDER;
                const ImVec4 col  = entry.IsFile ? entry.FileTypeColour : ImVec4( 0.95f, 0.82f, 0.42f, 1.0f );
                ImGui::PushFont( EditorResources::GetBigIconFont() );
                ImGui::TextColored( col, "%s", icon );
                ImGui::PopFont();
            }
            ImGui::SameLine();
            // Center the single-line filename against the preview block.
            ImGui::SetCursorPosY( ImGui::GetCursorPosY() +
                                  std::max( 0.0f, ( previewSize - ImGui::GetTextLineHeight() ) * 0.5f ) );
            ImGui::TextUnformatted( std::filesystem::path( assetPath ).filename().string().c_str() );
            ImGui::EndDragDropSource();
        }
    }

    bool FileExplorerPanel::DrawThumbnailFor( DirectoryInformation* entry, const ImVec2& size )
    {
        using ThumbnailProducers::Producer;
        const std::optional<Producer> producer = ThumbnailProducers::ProducerOf( entry->Type );
        if ( !producer )
            return false; // a kind with no row: ThumbnailProducers' census names it
        switch ( *producer )
        {
            case Producer::Decoded:
                return DrawTextureThumbnail( entry, size );
            case Producer::RenderedMaterial:
                return DrawRenderedMaterialThumbnail( entry, size );
            case Producer::RenderedMesh:
                return DrawRenderedMeshThumbnail( entry, size );
            case Producer::RenderedPose:
                return DrawRenderedPoseThumbnail( entry, size, entry->AssetPath );
            case Producer::Painted:
                return DrawPaintedThumbnail( entry, size );
            case Producer::RenderedSky:
            {
                // The Details Skybox row asks the same request with the same key: one picture per skybox.
                const Assets::AssetHandle skybox = Runtime::SkyboxHandleAtPath( entry->AssetPath );
                if ( static_cast<uint64_t>( skybox ) == 0 )
                    return false;
                const std::string png = ThumbnailService::Get().RequestSkybox( skybox, entry->AssetPath );
                if ( ThumbnailService::JudgeSkyboxPicture( entry->AssetPath ) !=
                     ThumbnailFreshness::Verdict::Show )
                {
                    m_Thumbnails->Invalidate( png );
                    return false;
                }
                if ( auto img = m_Thumbnails->Get( png ) )
                {
                    m_UIHelper->ImageButton( "##thumb", img, size );
                    return true;
                }
                return false;
            }
            case Producer::NotYetProduced:
            case Producer::TypeIcon:
                return false;
        }
        return false;
    }

    bool FileExplorerPanel::DrawTextureThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper || !m_Thumbnails )
            return false;

        // Decode the source image directly (cached), independent of the cook pipeline — so EVERY image
        // previews, not just already-cooked ones.
        auto img = m_Thumbnails->Get( entry->AssetPath );
        if ( !img )
            return false;

        // ImageButton (not Image) so the thumbnail is a real interactive item and can be a drag source.
        m_UIHelper->ImageButton( "##thumb", img, size );
        return true;
    }

    bool FileExplorerPanel::DrawRenderedMaterialThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper || !m_Thumbnails || m_AssetManager == nullptr )
            return false;

        // Cache PNG path: <versioned thumbnail dir>/<sanitized source path>.png (persists across restarts).
        const std::string& pngPath = ThumbnailPngFor( entry->AssetPath );

        // Through Editor/Widgets/ThumbnailFreshness.hpp, the same rule ThumbnailService::ShouldQueue applies:
        // Judge says whether a capture is owed, Choose says what to draw meanwhile. The PNG is drawn FIRST,
        // before the material is resolved or loaded — a card whose picture is on disk never waits for the
        // asset, and an outdated picture stays on screen until its replacement lands (ThumbnailCache::Get
        // re-decodes the rewritten file), instead of a flat albedo swatch for the whole queue.
        const ThumbnailFreshness::Observation seen = ThumbnailFreshness::Observe( pngPath, entry->AssetPath );
        const bool owed = ThumbnailFreshness::Judge( seen ) == ThumbnailFreshness::Verdict::Capture;
        bool       drew = false;
        if ( ThumbnailFreshness::Choose( seen ) == ThumbnailFreshness::Picture::CachedPng )
        {
            if ( auto img = m_Thumbnails->Get( pngPath ) )
            {
                m_UIHelper->ImageButton( "##thumb", img, size );
                drew = true;
            }
        }
        if ( drew && !owed )
        {
            m_CaptureAsked.erase( entry->AssetPath ); // current: a later edit that makes it stale asks again
            return true;
        }
        if ( const auto asked = m_CaptureAsked.find( entry->AssetPath ); asked != m_CaptureAsked.end() )
        {
            if ( !drew )
                ImGui::ColorButton( "##matswatch", asked->second,
                                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                                         ImGuiColorEditFlags_NoBorder,
                                    size );
            return true;
        }

        // Resolve material -> handle (load + register so the offscreen render can use it). Through
        // Editor/Widgets/ThumbnailSubject.hpp, which is the SAME resolution the background sweep uses —
        // the two used to be one copy each, and "which file is photographed" is exactly the question this
        // subsystem has already answered twice and differently once.
        //
        // PENDING IS A FRAME OR TWO OF THE PLACEHOLDER: the material is being read on a worker, and when
        // it lands the arrival delegate queues the capture exactly as the line below would have.
        const auto subject = ThumbnailSubject::ResolveMaterial(
             *m_AssetManager, entry->AssetPath,
             []( const std::string& assetPath, const Common::ResultStr<ThumbnailSubject::Material>& resolved )
             {
                 if ( resolved )
                     ThumbnailService::Get().RequestMaterial( resolved.GetValue(), assetPath );
                 else
                     ThumbnailService::Get().Refuse( assetPath, resolved.GetError() );
             } );
        // A REFUSAL IS SAID, not dropped (THM1n-10): M_CubemapCheck (Skybox domain) and M_CheckerFloor_Inst
        // kept a document icon with no request and no line in the log. The card still shows its icon; the
        // log names why, once per asset.
        if ( !subject )
        {
            ThumbnailService::Get().Refuse( entry->AssetPath, subject.GetError() );
            return drew;
        }
        const auto& material = subject.GetValue();
        if ( !material )
            return drew;

        auto a = m_AssetManager->FindByPath<Assets::SurfaceMaterialAsset>( entry->AssetPath );
        if ( !a )
            return drew;

        // Queue through the editor-wide service: it owns the one renderer, deduplicates against what other
        // panels already asked for, skips anything already on disk and never retries an asset that failed.
        ThumbnailService::Get().RequestMaterial( *material, entry->AssetPath );

        // No picture of this material exists yet: the albedo colour is the placeholder.
        const glm::vec3 albedo =
             glm::vec3( a->Data().GetParam( "AlbedoColor", glm::vec4( 0.8f, 0.8f, 0.8f, 1.0f ) ) );
        const ImVec4 swatch( albedo.r, albedo.g, albedo.b, 1.0f );
        m_CaptureAsked.emplace( entry->AssetPath, swatch );
        if ( drew )
            return true;
        ImGui::ColorButton(
             "##matswatch", swatch,
             ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoBorder, size );
        return true;
    }

    bool FileExplorerPanel::DrawRenderedMeshThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper || !m_Thumbnails || m_AssetManager == nullptr )
            return false;
        if ( m_FailedThumbs.contains( entry->AssetPath ) ) // failed to load before -> icon, no per-frame retry
            return false;

        // ONE PICTURE, ONE KEY, AND THE KEY IS THE FILE THAT IS ACTUALLY PHOTOGRAPHED.
        //
        // This grid used to file a mesh's thumbnail under its SOURCE (`assets:Meshes/x.fbx`) while the
        // Details 3D Model row files it under the COOKED form (`cooked:Meshes/x.stmesh`) — because a scene
        // holds the cooked handle and nothing else. Same mesh, same render, two cache files: a mesh both
        // browsed and placed in a scene was photographed TWICE, at 370 ms and ~200 KB a time, and neither
        // capture could ever satisfy the other panel.
        //
        // Cooked is the side that had to win, and not merely because the Details row cannot reach the
        // source. Freshness is a comparison against the recipe, and the recipe for this picture is the
        // .stmesh — StaticMeshAsset::Load reads cooked JSON and never opens the FBX. Judging against the
        // source asked whether a file the capture never reads had changed: re-cooking an unchanged FBX left
        // a stale picture called fresh, and touching an FBX without re-cooking threw away a picture that
        // still matched the geometry exactly.
        //
        // The mapping is a pure path computation (CookPaths::MeshAsset — an extension swap, no stat), so
        // hoisting it above the freshness check costs nothing; the `exists()` gate that decides "not cooked
        // -> icon" stays where it was, below, because that one IS a filesystem question.
        const std::optional<MeshPicture> picture = MeshPictureFor( entry->AssetPath, entry->Type );
        if ( !picture )
            return false; // not imported, clips only, or refused and named: the type icon
        if ( picture->Pose )
            return DrawRenderedPoseThumbnail( entry, size,
                                              picture->Cooked );           // a skinned source: its import's pose
        const std::optional<std::string> source = MeshSourceFor( *entry ); // a model, or a foliage type's mesh
        if ( !source )
            return false;
        const std::string& cookedStr = picture->Cooked;

        const std::string pngPath = ThumbnailKey::DiskPath( cookedStr );

        // The one verdict of a mesh picture (ThumbnailService::JudgeMeshPicture): the enqueue gate's key and hash.
        const bool haveFresh =
             ThumbnailService::JudgeMeshPicture( cookedStr ) == ThumbnailFreshness::Verdict::Show;
        if ( !haveFresh )
            m_Thumbnails->Invalidate( pngPath );
        if ( haveFresh )
        {
            if ( auto img = m_Thumbnails->Get( pngPath ) )
            {
                m_UIHelper->ImageButton( "##thumb", img, size );
                return true;
            }
        }

        // Cook lookup, build and the three refusals now live in Editor/Widgets/ThumbnailSubject.hpp, so
        // this tile and the background sweep resolve a mesh the same way. Every one of those refusals is
        // permanent for the session here — an uncooked source, a cooked file that will not build, a mesh
        // with no drawable submeshes — so the blacklist keeps the (logging) retry from happening once per
        // frame, exactly as it did when the code was in this function.
        const auto subject = ThumbnailSubject::ResolveMesh( *m_AssetManager, *source );
        if ( !subject )
        {
            // Once per asset (the blacklist stops the retry): a tile left on its type icon says why.
            LOG_WARN( "[Thumbnail] '{}': {}", entry->AssetPath, subject.GetError() );
            m_FailedThumbs.insert( entry->AssetPath );
            return false;
        }
        // Read in flight: the tile asks again next frame and meets it resident (never blacklisted).
        if ( subject.GetValue().Pending )
            return false;

        ThumbnailService::Get().RequestMesh( subject.GetValue().Handle, subject.GetValue().CookedPath,
                                             subject.GetValue().Material );

        // No swatch for meshes — fall back to the type icon until the PNG is ready.
        return false;
    }

    bool FileExplorerPanel::DrawRenderedPoseThumbnail( DirectoryInformation* entry, const ImVec2& size,
                                                       const std::string& subject )
    {
        if ( !m_UIHelper || !m_Thumbnails || m_AssetManager == nullptr )
            return false;
        if ( m_FailedThumbs.contains( entry->AssetPath ) ) // refused before -> icon, no per-frame retry
            return false;

        // The mesh tile's rule, with the .skmesh as its own cooked form: one key, one freshness source.
        const std::string& pngPath = ThumbnailPngFor( subject );
        const bool haveFresh = ThumbnailService::JudgeMeshPicture( subject ) == ThumbnailFreshness::Verdict::Show;
        if ( !haveFresh )
            m_Thumbnails->Invalidate( pngPath );
        else if ( auto img = m_Thumbnails->Get( pngPath ) )
        {
            m_UIHelper->ImageButton( "##thumb", img, size );
            return true;
        }

        const auto posed = ThumbnailPose::ResolvePoseSubject( *m_AssetManager, subject );
        if ( !posed )
        {
            LOG_WARN( "[Thumbnail] '{}': {}", entry->AssetPath, posed.GetError() );
            m_FailedThumbs.insert( entry->AssetPath );
            return false;
        }
        if ( posed.GetValue().Pending )
            return false; // read in flight: asked again next frame
        ThumbnailService::Get().RequestPose( posed.GetValue() );
        return false;
    }

    std::optional<std::string> FileExplorerPanel::MeshSourceFor( const DirectoryInformation& entry )
    {
        return MeshSourceFor( entry.AssetPath, entry.Type );
    }

    std::optional<std::string> FileExplorerPanel::MeshSourceFor( const std::string& assetPath,
                                                                 const FileType     type )
    {
        if ( type != FileType::FoliageType )
            return assetPath;
        if ( m_FailedThumbs.contains( assetPath ) )
            return std::nullopt;
        std::error_code                       ec;
        const std::filesystem::file_time_type written = std::filesystem::last_write_time( assetPath, ec );
        if ( const auto it = m_MeshSourceOf.find( assetPath );
             it != m_MeshSourceOf.end() && !ec && it->second.Written == written )
            return it->second.Source;
        const auto source = ThumbnailFoliage::ReadMeshSource( assetPath, Common::Constants::Path::ASSETS_PATH );
        if ( !source )
        {
            LOG_WARN( "[Thumbnail] '{}': {}", assetPath, source.GetError() );
            m_FailedThumbs.insert( assetPath );
            m_MeshSourceOf.erase( assetPath );
            return std::nullopt;
        }
        std::string mesh          = source.GetValue().generic_string();
        m_MeshSourceOf[assetPath] = { written, mesh };
        return mesh;
    }

    std::optional<FileExplorerPanel::MeshPicture> FileExplorerPanel::MeshPictureFor( const std::string& assetPath,
                                                                                     const FileType     type )
    {
        const std::optional<std::string> source = MeshSourceFor( assetPath, type );
        if ( !source )
            return std::nullopt;
        std::string extension = std::filesystem::path( *source ).extension().string();
        std::ranges::transform( extension, extension.begin(),
                                []( const unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        if ( std::ranges::find( Common::Content::kRawMeshSourceExtensions, extension ) ==
             Common::Content::kRawMeshSourceExtensions.end() )
            return MeshPicture{ CookPaths::MeshAsset( *source ).generic_string(), false }; // its own cooked form

        std::error_code                       ec;
        const std::filesystem::file_time_type written =
             std::filesystem::last_write_time( Common::Content::ImportRecordPathFor( *source ), ec );
        if ( ec )
            return std::nullopt; // not imported yet: a source is not an asset, its tile is the type icon
        if ( const auto it = m_SourcePictureOf.find( *source );
             it != m_SourcePictureOf.end() && it->second.Written == written )
            return it->second.Picture;

        std::optional<MeshPicture> picture;
        const auto                 kind = Assets::Serialization::ReadImportRecordKind( *source );
        if ( !kind )
        {
            LOG_WARN( "[Thumbnail] '{}': {}", assetPath, kind.GetError() ); // once per record version (cached)
        }
        else
        {
            using Common::Content::ContentKind;
            switch ( kind.GetValue() )
            {
                case ContentKind::StaticMesh:
                    picture = MeshPicture{ CookPaths::MeshAsset( *source ).generic_string(), false };
                    break;
                case ContentKind::SkinnedMesh:
                    picture = MeshPicture{ CookPaths::SkinnedAsset( *source, ".skmesh" ).generic_string(), true };
                    break;
                case ContentKind::Skeleton:
                    picture =
                         MeshPicture{ CookPaths::SkinnedAsset( *source, ".skeleton" ).generic_string(), true };
                    break;
                default:
                    break; // clips only: each clip has its own tile; the source keeps its type icon
            }
        }
        m_SourcePictureOf[*source] = { written, picture };
        return picture;
    }

    bool FileExplorerPanel::DrawPaintedThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper || !m_Thumbnails )
            return false;

        // NO ASSET MANAGER IN THIS FUNCTION, and that is the shape of the whole cloud path rather than an
        // oversight: the picture is computed from the file's own bytes, so nothing has to be created,
        // loaded or registered before it can be drawn. It is also why this tile keeps working in a
        // project whose asset layer has not finished starting.
        const std::string pngPath = ThumbnailKey::DiskPath( entry->AssetPath );

        const bool haveFresh =
             ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( pngPath, entry->AssetPath ) ) ==
             ThumbnailFreshness::Verdict::Show;
        if ( !haveFresh )
            m_Thumbnails->Invalidate( pngPath );

        if ( haveFresh )
        {
            if ( auto img = m_Thumbnails->Get( pngPath ) )
            {
                m_UIHelper->ImageButton( "##thumb", img, size );
                return true;
            }
        }

        ThumbnailService::Get().RequestPainted( entry->AssetPath );

        // The type icon until the PNG lands — no placeholder swatch, because unlike a material there is
        // no single colour that says anything true about a cloud volume.
        return false;
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
            const bool drewThumb = entry->IsFile && DrawThumbnailFor( entry, ImVec2( thumb, thumb ) );
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
            m_ItemMenu.Draw( *entry, m_CurrentDir );

            if ( ImGui::IsItemHovered() && !ImGui::IsDragDropActive() )
                DrawAssetTooltip( &*entry );
            else if ( m_TooltipEntry == &*entry )
                m_TooltipEntry = nullptr;

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

    void FileExplorerPanel::DrawAssetTooltip( DirectoryInformation* entry )
    {
        namespace Layout = Desert::Editor::AssetTooltipLayout;

        // The delay is ours because this ImGui (1.89 WIP) predates ImGuiHoveredFlags_DelayNormal.
        const double now = ImGui::GetTime();
        if ( m_TooltipEntry != entry )
        {
            m_TooltipEntry      = entry;
            m_TooltipHoverStart = now;
        }
        if ( !Layout::ShouldShow( static_cast<float>( now - m_TooltipHoverStart ) ) )
            return;

        const std::filesystem::path path( entry->AssetPath );
        const std::string           name     = path.filename().string();
        const char*                 typeName = entry->IsFile ? FileTypeInfoOf( entry->Type ).Name : "Folder";
        std::string                 sizeText;
        if ( entry->IsFile )
        {
            char buffer[32];
            if ( entry->FileSize >= std::size_t{ 1024 } * 1024 )
                std::snprintf( buffer, sizeof( buffer ), "%.1f MB", entry->FileSize / ( 1024.0f * 1024.0f ) );
            else
                std::snprintf( buffer, sizeof( buffer ), "%.1f KB", entry->FileSize / 1024.0f );
            sizeText = buffer;
        }
        // Shown relative to the PROJECT's directory (FPaths::ProjectDir), never to the working directory the
        // editor happened to be started from; a file outside the project (engine content in a foreign
        // project) keeps its full path rather than a chain of "..".
        std::error_code   ec;
        const std::string shownPath =
             std::filesystem::relative( path, Common::Constants::Path::ProjectDir(), ec ).generic_string();
        const bool         outside  = shownPath.starts_with( ".." );
        const std::string& pathText = ec || shownPath.empty() || outside ? entry->AssetPath : shownPath;

        // Natural size of the content: a 96 px picture beside name / type+size / path, the text wrapped
        // to what is left of the width cap. Layout::Compute then caps and places it on screen.
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2      thumbSize( 96.0f, 96.0f );
        const float       wrapWidth =
             Layout::kMaxWidth - thumbSize.x - style.ItemSpacing.x - 2.0f * style.WindowPadding.x;
        const float textH = ImGui::CalcTextSize( name.c_str(), nullptr, false, wrapWidth ).y +
                            ImGui::GetTextLineHeightWithSpacing() +
                            ImGui::CalcTextSize( pathText.c_str(), nullptr, false, wrapWidth ).y +
                            2.0f * style.ItemSpacing.y;
        const float wantedH = std::max( thumbSize.y, textH ) + 2.0f * style.WindowPadding.y;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const Layout::Rect   placed   = Layout::Compute(
             ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, Layout::kMaxWidth, wantedH,
             Layout::Rect{ viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y } );
        ImGui::SetNextWindowPos( ImVec2( placed.X, placed.Y ) );
        ImGui::SetNextWindowSize( ImVec2( placed.Width, placed.Height ) );
        ImGui::BeginTooltip();

        const bool drewThumb = DrawThumbnailFor( entry, thumbSize );
        if ( !drewThumb )
        {
            const char*  icon   = FileTypeInfoOf( entry->Type ).Icon;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 sz     = ImGui::CalcTextSize( icon );
            ImGui::GetWindowDrawList()->AddRectFilled( origin,
                                                       ImVec2( origin.x + thumbSize.x, origin.y + thumbSize.y ),
                                                       IM_COL32( 31, 31, 36, 255 ), 4.0f );
            ImGui::GetWindowDrawList()->AddText(
                 ImVec2( origin.x + ( thumbSize.x - sz.x ) * 0.5f, origin.y + ( thumbSize.y - sz.y ) * 0.5f ),
                 ImGui::GetColorU32( entry->FileTypeColour ), icon );
            ImGui::Dummy( thumbSize );
        }

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + wrapWidth );
        ImGui::TextUnformatted( name.c_str() );
        if ( sizeText.empty() )
            ImGui::TextDisabled( "%s", typeName );
        else
            ImGui::TextDisabled( "%s  |  %s", typeName, sizeText.c_str() );
        ImGui::TextDisabled( "%s", pathText.c_str() );
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        ImGui::EndTooltip();
    }

} // namespace Desert::Editor

static_assert( Common::HandlesEvent<Desert::Editor::FileExplorerPanel, Common::EventWindowFileDrop> );

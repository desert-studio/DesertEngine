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
#include <Engine/Assets/EnhancedInputAssets.hpp>
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

<<<<<<< HEAD
    FileExplorerPanel::FileExplorerPanel( const std::filesystem::path& rootPath,
                                          const SubjectEditorRegistry* subjectEditors,
                                          AssetThumbnailPool& thumbnailPool, Assets::AssetManager* assetManager,
=======
    namespace
    {
        // THE ONE ROUTE FOR A RENAME OR A MOVE (AF10c). Content the registry has a row for moves through it:
        // a redirector stays at the old path, so every scene still naming that path keeps loading, and the
        // move lands on the undo stack. A folder moves the same way for every row inside it, all or nothing,
        // as one undo step. Only a loose file the registry does not know (a source image, a note) is a plain
        // file operation - nothing names it by path through the registry.
        bool MoveOrRename( const std::string& src, const std::filesystem::path& dst, const char* label,
                           std::string& error )
        {
            std::error_code ec;
            const bool      folder = std::filesystem::is_directory( src, ec );
            if ( folder || Assets::ContentRegistry::HasRow( src ) )
            {
                const auto moved =
                     folder ? MoveFolderWithUndo( src, dst, label ) : MoveAssetWithUndo( src, dst, label );
                if ( !moved )
                    error = moved.GetError();
                return static_cast<bool>( moved );
            }
            std::string newPath;
            return dst.parent_path() == std::filesystem::path( src ).parent_path()
                        ? AssetFileOps::Rename( src, dst.filename().string(), newPath, error )
                        : AssetFileOps::Move( src, dst.parent_path().string(), newPath, error );
        }

        // A drag onto a folder: the file keeps its name in the destination DIRECTORY.
        bool MoveFileTo( const std::string& filePath, const std::string& movePath )
        {
            std::string error;
            const bool  moved = MoveOrRename(
                 filePath, std::filesystem::path( movePath ) / std::filesystem::path( filePath ).filename(),
                 "Move", error );
            if ( !moved )
                LOG_ERROR( "[Assets] move '{}' -> '{}': {}", filePath, movePath, error );
            return moved;
        }

        std::string ToLowerCopy( std::string s )
        {
            std::transform( s.begin(), s.end(), s.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return s;
        }

        // --- OS shell integration (no-ops on unsupported platforms) ---------------------------------------
        void ShellOpenDefault( const std::string& path )
        {
#if defined( DESERT_PLATFORM_WINDOWS )
            std::error_code ec;
            const auto      abs = std::filesystem::absolute( path, ec ).make_preferred().wstring();
            ShellExecuteW( nullptr, L"open", abs.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
#elif defined( DESERT_PLATFORM_MACOS )
            std::error_code   ec;
            const std::string abs = std::filesystem::absolute( path, ec ).string();
            const std::string cmd = "open \"" + abs + "\"";
            system( cmd.c_str() );
#else
            (void)path;
#endif
        }

        // Open Explorer/Finder with the item selected (file or folder highlighted in its parent).
        void ShellRevealInExplorer( const std::string& path )
        {
#if defined( DESERT_PLATFORM_WINDOWS )
            std::error_code    ec;
            const auto         abs    = std::filesystem::absolute( path, ec ).make_preferred().wstring();
            const std::wstring params = L"/select,\"" + abs + L"\"";
            ShellExecuteW( nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL );
#elif defined( DESERT_PLATFORM_MACOS )
            std::error_code   ec;
            const std::string abs = std::filesystem::absolute( path, ec ).string();
            const std::string cmd = "open -R \"" + abs + "\"";
            system( cmd.c_str() );
#else
            (void)path;
#endif
        }
    } // namespace

    static const std::unordered_map<FileType, std::string> s_FileTypesToString = {
         { FileType::Unknown, "Unknown" },
         { FileType::Scene, "Scene" },
         { FileType::Prefab, "Prefab" },
         { FileType::Script, "Script" },
         { FileType::Shader, "Shader" },
         { FileType::Texture, "Texture" },
         { FileType::Font, "Font" },
         { FileType::Cubemap, "Cubemap" },
         { FileType::Model, "Model" },
         { FileType::Audio, "Audio" },
         { FileType::Material, "Material" },
         { FileType::ShaderGraph, "Shader Graph" },
         { FileType::Cloud, "Cloud" },
         { FileType::ImportSettings, "Import Settings" },
         { FileType::UITheme, "UI Theme" },
         { FileType::LandscapeLayerInfo, "Landscape Layer Info" },
         { FileType::LevelSequence, "Level Sequence" },
         { FileType::VFXSystem, "VFX System" },
         { FileType::Fracture, "Fracture" },
         { FileType::Ini, "Settings" },
         { FileType::SkinnedMesh, "Skeletal Mesh" },
         { FileType::Skeleton, "Skeleton" },
         { FileType::Animation, "Animation" },
         { FileType::ControlRig, "Control Rig" },
         { FileType::AnimGraph, "Anim Graph" },
         { FileType::Retarget, "Retarget" },
         { FileType::FoliageType, "Foliage Type" },
         { FileType::StringTable, "String Table" },
         { FileType::InputAction, "Input Action" },
         { FileType::InputMappingContext, "Input Mapping Context" },
         { FileType::CookedWorld, "Cooked World" },
         { FileType::Skybox, "Skybox" },
    };

    static const std::unordered_map<FileType, ImVec4> s_TypeColors = {
         { FileType::Scene, { 0.8f, 0.4f, 0.22f, 1.00f } },
         { FileType::Prefab, { 0.10f, 0.50f, 0.80f, 1.00f } },
         { FileType::Script, { 0.10f, 0.50f, 0.80f, 1.00f } },
         { FileType::Font, { 0.60f, 0.19f, 0.32f, 1.00f } },
         { FileType::Shader, { 0.10f, 0.50f, 0.80f, 1.00f } },
         { FileType::Texture, { 0.82f, 0.20f, 0.33f, 1.00f } },
         { FileType::Cubemap, { 0.82f, 0.18f, 0.30f, 1.00f } },
         { FileType::Model, { 0.18f, 0.82f, 0.76f, 1.00f } },
         { FileType::Audio, { 0.20f, 0.80f, 0.50f, 1.00f } },
         { FileType::ShaderGraph, { 0.55f, 0.35f, 0.85f, 1.00f } },
         { FileType::Cloud, { 0.62f, 0.78f, 0.95f, 1.00f } },
         { FileType::Ini, { 0.65f, 0.65f, 0.68f, 1.00f } },
         { FileType::UITheme, { 0.95f, 0.72f, 0.30f, 1.00f } },
         { FileType::LandscapeLayerInfo, { 0.45f, 0.70f, 0.30f, 1.00f } },
         { FileType::LevelSequence, { 0.85f, 0.35f, 0.25f, 1.00f } },
         { FileType::VFXSystem, { 0.95f, 0.45f, 0.10f, 1.00f } },
         { FileType::Fracture, { 0.75f, 0.55f, 0.35f, 1.00f } },
         { FileType::ImportSettings, { 0.65f, 0.65f, 0.68f, 1.00f } },
         // UE's class colours for the animation family, so a folder of rig content reads as one family.
         { FileType::SkinnedMesh, { 0.90f, 0.35f, 0.90f, 1.00f } },
         { FileType::Skeleton, { 0.41f, 0.71f, 0.80f, 1.00f } },
         { FileType::Animation, { 0.31f, 0.70f, 0.28f, 1.00f } },
         { FileType::ControlRig, { 0.20f, 0.45f, 0.95f, 1.00f } },
         { FileType::AnimGraph, { 0.80f, 0.55f, 0.20f, 1.00f } },
         { FileType::Retarget, { 0.95f, 0.50f, 0.60f, 1.00f } },
         { FileType::FoliageType, { 0.30f, 0.75f, 0.35f, 1.00f } },
         { FileType::StringTable, { 0.60f, 0.60f, 0.85f, 1.00f } },
         { FileType::InputAction, { 0.35f, 0.80f, 0.45f, 1.00f } },
         { FileType::InputMappingContext, { 0.20f, 0.65f, 0.55f, 1.00f } },
         { FileType::CookedWorld, { 0.50f, 0.50f, 0.55f, 1.00f } },
         { FileType::Skybox, { 0.82f, 0.18f, 0.30f, 1.00f } },
    };

    static const std::unordered_map<FileType, const char*> s_FileTypesToIcon = {
         { FileType::Unknown, ICON_MDI_FILE },
         { FileType::Scene, ICON_MDI_FILE },
         { FileType::Prefab, ICON_MDI_FILE },
         { FileType::Script, ICON_MDI_LANGUAGE_LUA },
         { FileType::Shader, ICON_MDI_IMAGE_FILTER_BLACK_WHITE },
         { FileType::Texture, ICON_MDI_FILE_IMAGE },
         { FileType::Font, ICON_MDI_FORMAT_FONT },
         { FileType::Cubemap, ICON_MDI_IMAGE_FILTER_HDR },
         { FileType::Model, ICON_MDI_VECTOR_POLYGON },
         { FileType::Audio, ICON_MDI_MICROPHONE },
         { FileType::ShaderGraph, ICON_MDI_GRAPH },
         // The same glyph the cloud-type document registers itself with (EditorLayer's subject-editor
         // registration), so the browser tile and the window it opens are recognisably the same thing.
         { FileType::Cloud, ICON_MDI_WEATHER_CLOUDY },
         { FileType::Ini, ICON_MDI_FILE_DOCUMENT },
         { FileType::UITheme, ICON_MDI_PALETTE },
         { FileType::LandscapeLayerInfo, ICON_MDI_LAYERS },
         { FileType::LevelSequence, ICON_MDI_MOVIE_OPEN },
         { FileType::VFXSystem, ICON_MDI_FIRE },
         { FileType::Fracture, ICON_MDI_CUBE_UNFOLDED },
         { FileType::ImportSettings, ICON_MDI_FILE_DOCUMENT },
         { FileType::SkinnedMesh, ICON_MDI_HUMAN },
         { FileType::Skeleton, ICON_MDI_BONE },
         { FileType::Animation, ICON_MDI_RUN },
         { FileType::ControlRig, ICON_MDI_HUMAN_HANDSUP },
         { FileType::AnimGraph, ICON_MDI_SITEMAP },
         { FileType::Retarget, ICON_MDI_SWAP_HORIZONTAL },
         { FileType::FoliageType, ICON_MDI_TREE },
         { FileType::StringTable, ICON_MDI_TRANSLATE },
         { FileType::InputAction, ICON_MDI_GESTURE_TAP },
         { FileType::InputMappingContext, ICON_MDI_KEYBOARD },
         { FileType::CookedWorld, ICON_MDI_MAP },
         { FileType::Skybox, ICON_MDI_IMAGE_FILTER_HDR },
    };

    FileExplorerPanel::FileExplorerPanel( const std::filesystem::path&         rootPath,
                                          const SubjectEditorRegistry*         subjectEditors,
                                          Assets::AssetManager*                assetManager,
>>>>>>> origin/task/GP2
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

<<<<<<< HEAD
=======
    Common::BoolResultStr FileExplorerPanel::CreateNewInputAction()
    {
        if ( m_CurrentDir == nullptr )
            return Common::MakeError( "New Input Action: the Assets window has no folder open" );
        const std::filesystem::path folder( m_CurrentDir->AssetPath );
        const std::string           name = AssetFileOps::UniqueName(
             "IA_NewAction", Assets::Serialization::kInputActionExtension,
             [&]( const std::string& n ) { return std::filesystem::exists( folder / n ); } );
        const auto path = folder / name;
        if ( const auto saved = Assets::InputActionAsset::Save( path, Assets::Serialization::InputActionData{} );
             !saved )
            return Common::MakeFormattedError( "New Input Action: {}", saved.GetError() );
        m_SelectAfterRefresh = path.generic_string();
        QueueRefresh();
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr FileExplorerPanel::CreateNewInputMappingContext()
    {
        if ( m_CurrentDir == nullptr )
            return Common::MakeError( "New Input Mapping Context: the Assets window has no folder open" );
        const std::filesystem::path folder( m_CurrentDir->AssetPath );
        const std::string           name = AssetFileOps::UniqueName(
             "IMC_NewContext", Assets::Serialization::kInputMappingContextExtension,
             [&]( const std::string& n ) { return std::filesystem::exists( folder / n ); } );
        const auto path = folder / name;
        if ( const auto saved =
                  Assets::InputMappingContextAsset::Save( path, Assets::Serialization::InputMappingContextData{} );
             !saved )
            return Common::MakeFormattedError( "New Input Mapping Context: {}", saved.GetError() );
        m_SelectAfterRefresh = path.generic_string();
        QueueRefresh();
        return Common::MakeSuccess( true );
    }

    void FileExplorerPanel::CreateNewCloudAsset( CloudAssetKind kind )
    {
        if ( !m_CurrentDir )
            return;

        // The menu items are disabled while a generation is in flight; this is the second lock and it is
        // not redundant, because overwriting m_CloudBake would detach a thread still storing into
        // m_CloudBakeProgress.
        if ( m_CloudBakeRunning )
            return;

        const char* stem = nullptr;
        const char* ext  = nullptr;
        switch ( kind )
        {
            case CloudAssetKind::Type:
                stem = "NewCloudType";
                ext  = Assets::kCloudTypeExtension;
                break;
            case CloudAssetKind::Layout:
                stem = "NewCloudLayout";
                ext  = Assets::kCloudLayoutExtension;
                break;
            case CloudAssetKind::NoiseVolume:
                stem = "NewCloudNoise";
                ext  = Assets::kCloudNoiseVolumeExtension;
                break;
            case CloudAssetKind::ModellingVolume:
                stem = "NewCloudBody";
                ext  = Assets::kCloudModellingVolumeExtension;
                break;
        }

        // The same uniquifier the material item uses, so creating a second one never silently overwrites
        // somebody's file.
        const std::string name = AssetFileOps::UniqueName(
             stem, ext, [&]( const std::string& n )
             { return std::filesystem::exists( std::filesystem::path( m_CurrentDir->AssetPath ) / n ); } );

        // THE DIRECTORY IS THE ONE THE ARTIST IS LOOKING AT, not Constants::Path::CLOUD_*_PATH. Those name
        // where the SHIPPED library lives and are what a scene resolves a preset against; where somebody
        // puts their own asset is their business, and the same choice the material item already makes.
        const std::filesystem::path path = std::filesystem::path( m_CurrentDir->AssetPath ) / name;

        m_CloudBakePath  = path.string();
        m_CloudBakeLabel = name;

        // ── The two that are numbers, and are written where they were asked for ────────────────────────
        //
        // Through the format's own `Save` and NOT through a literal, which is what CreateNewMaterial above
        // does: it writes `{"Params":[],"Textures":[]}` straight out, past the serialiser that every other
        // writer of a `.demat` goes through. That is a second statement of a file format, and a second
        // statement drifts — the material the editor saves after touching one already carries parameter
        // entries (`AOStrength` among them) that this literal has never heard of. All four cloud formats
        // have a real `Save`, so there is exactly one statement of each of them and this is not the place
        // to add a fifth.
        if ( kind == CloudAssetKind::Type )
        {
            FinishCloudAsset(
                 Assets::CloudTypeAsset::Save( path, NewCloudAsset::DefaultType( path.stem().string() ) ) );
            return;
        }

        if ( kind == CloudAssetKind::Layout )
        {
            auto layout = NewCloudAsset::DefaultLayout();
            if ( !layout )
            {
                FinishCloudAsset( Common::MakeFormattedError<bool>( "{}", layout.GetError() ) );
                return;
            }

            FinishCloudAsset( Assets::CloudLayoutAsset::Save( path, layout.GetValue() ) );
            return;
        }

        // ── The two that are voxels, and cost seconds ──────────────────────────────────────────────────
        //
        // MEASURED, NOT REASONED (Debug, this machine, minimum of three interleaved runs): the default
        // 128^3 noise volume takes 8 730 ms to generate (spread 255 ms) and the shipped modelling recipe
        // 1 585 ms (spread 9 ms). Both are three orders of magnitude past a frame, so neither can run in
        // this handler — an editor that stops answering for nine seconds is indistinguishable from one
        // that has hung, and the artist's next move is to click the item again.
        //
        // std::async, matching CloudNoiseVolumePanel and CloudModellingVolumePanel, which run these same
        // two bakes this same way: copying how the neighbouring systems do it rather than inventing a
        // third way. The JobSystem would buy nothing here either — GenerateCloudNoiseVolume already splits
        // itself across one thread per hardware thread, so a pool worker would only nest two pools.
        m_CloudBakeProgress.store( 0.0f );
        m_CloudBakeCancelled.store( false );
        m_CloudBakeRunning = true;

        m_CloudBake = std::async( std::launch::async,
                                  [this, path, kind]() -> Common::BoolResultStr
                                  {
                                      // SAVED ON THE WORKER, and it is safe for a reason worth stating: all four
                                      // `Save`s are pure file I/O plus a log line — no AssetManager, no ECS, no
                                      // GPU — which is exactly the set a job is forbidden to touch. Handing 8 MiB
                                      // back to the main thread to write there would only move the disk stall into
                                      // the frame.
                                      if ( kind == CloudAssetKind::NoiseVolume )
                                      {
                                          auto volume = NewCloudAsset::DefaultNoiseVolume( &m_CloudBakeProgress );
                                          if ( !volume )
                                              return Common::MakeFormattedError<bool>( "{}", volume.GetError() );

                                          return Assets::CloudNoiseVolumeAsset::Save( path, volume.GetValue() );
                                      }

                                      auto body = NewCloudAsset::DefaultModellingVolume(
                                           [this]( float fraction )
                                           {
                                               m_CloudBakeProgress.store( fraction );
                                               return !m_CloudBakeCancelled.load();
                                           } );
                                      if ( !body )
                                          return Common::MakeFormattedError<bool>( "{}", body.GetError() );

                                      return Assets::CloudModellingVolumeAsset::Save( path, body.GetValue() );
                                  } );
    }

    void FileExplorerPanel::PollCloudAssetBake()
    {
        if ( !m_CloudBakeRunning || !m_CloudBake.valid() )
            return;

        if ( m_CloudBake.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            return;

        const Common::BoolResultStr written = m_CloudBake.get();
        m_CloudBakeRunning                  = false;
        FinishCloudAsset( written );
    }

    void FileExplorerPanel::FinishCloudAsset( const Common::BoolResultStr& written )
    {
        if ( !written )
        {
            // NEVER SILENT (contract §1.4). `Save` refuses an unwritable directory, a full disk and data
            // that would not load back, each with the reason; a "New ..." item that sometimes produces no
            // file and says nothing is worse than no item at all.
            LOG_ERROR( "[Assets] '{}' could not be created: {}", m_CloudBakePath, written.GetError() );
            m_FileOpStatus = "Could not create '" + m_CloudBakeLabel + "': " + written.GetError();
            return;
        }

        QueueRefresh();

        // OPENED STRAIGHT AWAY, because creating one of these is the only way to reach its editor at all:
        // the four cloud documents are contextual, keyed on an asset handle, and have no View-menu entry,
        // so until a file exists there is nothing for the double-click seam to open. RequestCloudDocument
        // logs its own failures with the path.
        if ( m_AssetManager &&
             RequestCloudDocument( m_AssetManager, m_CloudBakePath ) != CloudDocumentRequest::Requested )
        {
            m_FileOpStatus = "Created '" + m_CloudBakeLabel + "' but it would not open — the log says why.";
            return;
        }

        // The status line is the RED error line; a success has the file, the opened document and Save's own
        // log entry to show for itself, so it clears rather than colours one.
        m_FileOpStatus.clear();
    }

    void FileExplorerPanel::DrawCloudAssetBakeStatus()
    {
        if ( !m_CloudBakeRunning )
            return;

        const std::string line = "Creating '" + m_CloudBakeLabel + "' - this takes a few seconds.";
        ImGui::TextUnformatted( line.c_str() );
        ImGui::ProgressBar( m_CloudBakeProgress.load(), ImVec2( -1.0f, 0.0f ) );
    }

    void FileExplorerPanel::RemoveDirectoryNode( DirectoryInformation* directory, bool removeFromParent )
    {
        if ( directory->Parent && removeFromParent )
        {
            directory->Parent->Children.clear();
        }

        // A walk, not recursion: the tree is as deep as the user's folders.
        std::vector<DirectoryInformation*> pending{ directory };
        while ( !pending.empty() )
        {
            DirectoryInformation* node = pending.back();
            pending.pop_back();
            pending.insert( pending.end(), node->Children.begin(), node->Children.end() );
            m_Directories.erase( node->AssetPath ); // may destroy `node`; its children were taken first
        }
    }

    // Unreadable, or the platform's own junk. A path that cannot be STAT'd is not hidden — it is a
    // browser row we know nothing about, and treating it as visible is the honest answer.
    //
    // The error is taken through the std::error_code overload rather than a catch. A try/catch stood
    // here whose entire handler was a commented-out LOG_ERROR, so a failed status() was swallowed with
    // no diagnostic reachable even in principle: the one line that would have said something had been
    // turned into text (§1.4). The error_code form cannot be silent by accident, because the failure is
    // a value this function has to read.
    bool IsHidden( const std::filesystem::path& filePath )
    {
        std::error_code                    ec;
        const std::filesystem::file_status status = std::filesystem::status( filePath, ec );
        if ( ec )
            return false;

        return ( status.permissions() & std::filesystem::perms::owner_read ) == std::filesystem::perms::none ||
               filePath.stem().string() == ".DS_Store";
    }

    std::string FileExplorerPanel::ProcessDirectory( const std::string&    directoryPath,
                                                     DirectoryInformation* parent, bool processChildren )
    {
        const auto& directory = m_Directories[directoryPath];
        if ( directory && directory->Opened )
            return directory->AssetPath;

        // The path AS THE CALLER SPELLED IT is the node's identity: it is the key in m_Directories, the
        // string the navigation history stores, and what every child is built from. Three lines here
        // said otherwise — an `absolutePath` alias annotated "replace with actual path resolution", the
        // same note on the assignment below, and a standing marker about pooling the strings — and they
        // described an intention nobody has held for as long as the file has existed. A note that
        // promises a different design is read as one, and this panel's real remainder is not string
        // storage: it is that the whole model is DISK-shaped, which is what its row in
        // Tests/Common/ContentScanners records with the measurement behind it.
        const std::filesystem::path stdPath( directoryPath );

        std::shared_ptr<DirectoryInformation> directoryInfo =
             directory ? directory
                       : std::make_shared<DirectoryInformation>( directoryPath,
                                                                 !std::filesystem::is_directory( stdPath ) );
        directoryInfo->Parent    = parent;
        directoryInfo->AssetPath = directoryPath;

        std::string extension = stdPath.extension().string();
        if ( !extension.empty() && extension[0] == '.' )
            extension = extension.substr( 1 );

        if ( std::filesystem::is_directory( stdPath ) )
        {
            directoryInfo->IsFile = false;
            directoryInfo->Leaf   = true;
            for ( auto& entry : std::filesystem::directory_iterator( stdPath ) )
            {
                if ( !m_ShowHiddenFiles && IsHidden( entry.path() ) )
                    continue;

                // A branch that hid a cache folder used to stand here, testing AssetPath for a substring
                // beginning with a DOUBLE separator. Every path in this map comes from generic_string()
                // of a directory entry, which never produces one, so the condition could not be true —
                // and the folder it wanted to hide does not exist under any assets root in this project
                // either. Dead on both counts, and it also set Hidden on the PARENT while skipping a
                // CHILD, so had it ever fired it would have hidden the wrong node.
                if ( entry.is_directory() )
                    directoryInfo->Leaf = false;

                if ( processChildren )
                {
                    directoryInfo->Opened = true;

                    std::string subdirHandle =
                         ProcessDirectory( entry.path().generic_string(), directoryInfo.get(), false );
                    directoryInfo->Children.push_back( m_Directories[subdirHandle].get() );
                }
            }
        }
        else
        {
            // Root-aware: a `.detex` under the Skybox root is a Skybox, not a Texture (FileTypeOfContent).
            const FileType fileType =
                 FileTypeOfContent( extension, Common::Content::KindOfContentFile( stdPath ) );

            directoryInfo->IsFile = true;
            directoryInfo->Type   = fileType;
            directoryInfo->FileSize =
                 std::filesystem::exists( stdPath ) ? std::filesystem::file_size( stdPath ) : 0;
            {
                std::error_code wec;
                const auto      t            = std::filesystem::last_write_time( stdPath, wec );
                directoryInfo->LastWriteTime = wec ? 0 : static_cast<uint64_t>( t.time_since_epoch().count() );
            }
            directoryInfo->Hidden = std::filesystem::exists( stdPath ) ? IsHidden( stdPath ) : true;
            directoryInfo->Opened = true;
            directoryInfo->Leaf   = true;

            ImVec4      fileTypeColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
            const auto& fileTypeColorIt = s_TypeColors.find( fileType );
            if ( fileTypeColorIt != s_TypeColors.end() )
                fileTypeColor = fileTypeColorIt->second;

            directoryInfo->FileTypeColour = fileTypeColor;
        }

        if ( !directory )
            m_Directories[directoryInfo->AssetPath] = directoryInfo;
        return directoryInfo->AssetPath;
    }

    void FileExplorerPanel::DrawFolder( DirectoryInformation* dirInfo, bool defaultOpen )
    {
        ImGuiTreeNodeFlags nodeFlags = ( ( dirInfo == m_CurrentDir ) ? ImGuiTreeNodeFlags_Selected : 0 );
        nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;

        if ( dirInfo->Parent == nullptr )
            nodeFlags |= ImGuiTreeNodeFlags_Framed;

        const ImColor TreeLineColor = ImColor( 128, 128, 128, 128 );
        const float   SmallOffsetX  = 6.0f; // * Application::Get().GetWindowDPI();
        ImDrawList*   drawList      = ImGui::GetWindowDrawList();

        if ( !dirInfo->IsFile )
        {
            if ( dirInfo->Leaf )
                nodeFlags |= ImGuiTreeNodeFlags_Leaf;

            if ( defaultOpen )
                nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf;

            nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

            bool isOpen = ImGui::TreeNodeEx( (void*)(intptr_t)( dirInfo ), nodeFlags, "" );
            if ( ImGui::IsItemClicked() )
            {
                ChangeDirectory( dirInfo );
            }

            const char* folderIcon = ( ( isOpen && !dirInfo->Leaf ) || m_CurrentDir == dirInfo )
                                          ? ICON_MDI_FOLDER_OPEN
                                          : ICON_MDI_FOLDER;
            ImGui::SameLine();
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 1.0f, 1.0f, 1.0f ) ); // Replace with actual color
            ImGui::TextUnformatted( folderIcon );
            ImGui::PopStyleColor();
            ImGui::SameLine();

            std::string fileName = std::filesystem::path( dirInfo->AssetPath ).filename().string();
            ImGui::TextUnformatted( fileName.c_str() );

            ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();

            if ( isOpen && !dirInfo->Leaf )
            {
                verticalLineStart.x += SmallOffsetX; // to nicely line up with the arrow symbol
                ImVec2 verticalLineEnd = verticalLineStart;

                for ( size_t i = 0; i < dirInfo->Children.size(); i++ )
                {
                    if ( !m_ShowHiddenFiles && dirInfo->Children[i]->Hidden )
                    {
                        continue;
                    }

                    if ( !dirInfo->Children[i]->IsFile )
                    {
                        auto currentPos = ImGui::GetCursorScreenPos();

                        ImGui::Indent( 10.0f );

                        float HorizontalTreeLineSize =
                             16.0f; // * Application::Get().GetWindowDPI(); // chosen arbitrarily

                        if ( !dirInfo->Children[i]->Leaf )
                            HorizontalTreeLineSize *= 0.5f;
                        DrawFolder( dirInfo->Children[i] );

                        const ImRect childRect =
                             ImRect( currentPos, currentPos + ImVec2( 0.0f, ImGui::GetFontSize() ) );

                        const float midpoint = ( childRect.Min.y + childRect.Max.y ) * 0.5f;
                        drawList->AddLine( ImVec2( verticalLineStart.x, midpoint ),
                                           ImVec2( verticalLineStart.x + HorizontalTreeLineSize, midpoint ),
                                           TreeLineColor );
                        verticalLineEnd.y = midpoint;

                        ImGui::Unindent( 10.0f );
                    }
                }

                drawList->AddLine( verticalLineStart, verticalLineEnd, TreeLineColor );

                ImGui::TreePop();
            }

            if ( isOpen && dirInfo->Leaf )
                ImGui::TreePop();
        }

        if ( m_IsDragging && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem ) )
        {
            m_MovePath = dirInfo->AssetPath;
        }
    }

    static int FileIndex = 0;

>>>>>>> origin/task/GP2
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
<<<<<<< HEAD
                if ( const auto selected = SelectEntry( std::exchange( m_SelectAfterRefresh, {} ) ); !selected )
                    LOG_ERROR( "[Content] the new asset was created but not selected: {}", selected.GetError() );
=======
                RefreshCurrentDirectory(); // in-place: keeps navigation (watcher / import / rebuild)
                m_Refresh = false;
                if ( !m_SelectAfterRefresh.empty() )
                {
                    if ( const auto selected = SelectEntry( std::exchange( m_SelectAfterRefresh, {} ) );
                         !selected )
                        LOG_ERROR( "[Content] the new asset was created but not selected: {}",
                                   selected.GetError() );
                }
            }

            // ── Content Browser: two panes split by a draggable vertical splitter. LEFT = pinned Favorites
            //    + the project folder tree; RIGHT = toolbar / breadcrumb / asset grid / preview strip. ──
            constexpr float kMinTreeWidth    = 120.0f;
            constexpr float kMinContentWidth = 220.0f;
            constexpr float kSplitterW       = 6.0f;
            const float     totalAvail       = ImGui::GetContentRegionAvail().x;
            m_TreeWidth                      = std::clamp( m_TreeWidth, kMinTreeWidth,
                                                           std::max( kMinTreeWidth, totalAvail - kMinContentWidth - kSplitterW ) );

            // LEFT PANE.
            ImGui::BeginChild( "##cb_left", ImVec2( m_TreeWidth, 0.0f ), true );
            {
                // The pins OF THE OPEN PROJECT, asked for where they are drawn. A second project's
                // folders used to appear here, because the retired file had one list for every project
                // this user had ever opened (К5).
                const std::vector<std::string> favourites = EditorPreferences::CurrentFavouriteFolders();
                if ( !favourites.empty() )
                {
                    ImGui::TextDisabled( ICON_MDI_STAR " FAVORITES" );
                    for ( const auto& fav : favourites )
                    {
                        const std::string label = std::filesystem::path( fav ).filename().string();
                        ImGui::PushID( fav.c_str() );
                        if ( ImGui::Selectable(
                                  ( std::string( "  " ) + ICON_MDI_FOLDER " " + ( label.empty() ? fav : label ) )
                                       .c_str() ) )
                            NavigateToPath( fav );
                        if ( ImGui::BeginPopupContextItem( "##favctx" ) )
                        {
                            if ( ImGui::MenuItem( "Remove from Favorites" ) )
                                EditorPreferences::ToggleFavouriteFolder( fav );
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                }
                ImGui::TextDisabled( ICON_MDI_FOLDER_MULTIPLE " CONTENT" );
                DrawFolder( m_BaseProjectDir, true );
            }
            ImGui::EndChild();

            // The folder tree is a move-drop target: drag an asset onto a folder to move it there (the hovered
            // folder sets m_MovePath inside DrawFolder).
            if ( ImGui::BeginDragDropTarget() )
            {
                if ( auto data = ImGui::AcceptDragDropPayload( "selectable",
                                                               ImGuiDragDropFlags_AcceptNoDrawDefaultRect ) )
                {
                    std::string* file = (std::string*)data->Data;
                    MoveFileTo( *file, m_MovePath );
                    m_IsDragging = false;
                }
                ImGui::EndDragDropTarget();
            }

            // SPLITTER — a thin invisible handle the user drags to resize the tree pane.
            ImGui::SameLine( 0.0f, 0.0f );
            // GetContentRegionAvail().y can be 0 on a first/zero-height frame; InvisibleButton asserts on a
            // zero size, so floor the height at 1px (harmless — the handle is invisible anyway).
            const float cbSplitterH = ImGui::GetContentRegionAvail().y;
            ImGui::InvisibleButton( "##cb_splitter",
                                    ImVec2( kSplitterW, cbSplitterH > 0.0f ? cbSplitterH : 1.0f ) );
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

            // Toolbar strip (settings / search / sort / filter / nav / import + breadcrumb) inside the right pane.
            {
                {
                    ImGui::BeginChild( "##cb_toolbar", ImVec2( 0.0f, ImGui::GetFrameHeightWithSpacing() * 2.0f ),
                                       false, ImGuiWindowFlags_NoScrollbar );

                    ImGui::AlignTextToFramePadding();
                    // Button for advanced settings
                    {
                        // Replace with actual style color
                        if ( ImGui::Button( ICON_MDI_COGS ) )
                            ImGui::OpenPopup( "SettingsPopup" );
                    }
                    if ( ImGui::BeginPopup( "SettingsPopup" ) )
                    {
                        if ( m_IsInListView )
                        {
                            if ( ImGui::Button( ICON_MDI_VIEW_LIST " Switch to Grid View" ) )
                            {
                                m_IsInListView = !m_IsInListView;
                            }
                        }
                        else
                        {
                            if ( ImGui::Button( ICON_MDI_VIEW_GRID " Switch to List View" ) )
                            {
                                m_IsInListView = !m_IsInListView;
                            }
                        }

                        if ( ImGui::Selectable( "Refresh" ) )
                        {
                            QueueRefresh();
                        }

                        if ( ImGui::Selectable( "New folder" ) )
                        {
                            std::string fullPath = m_CurrentDir->AssetPath + "/NewFolder";
                            std::filesystem::create_directory( fullPath );
                            QueueRefresh();
                        }

                        if ( !m_IsInListView )
                        {
                            ImGui::SliderFloat( "##GridSize", &m_GridSize, 40.0f, 400.0f );
                        }

                        ImGui::EndPopup();
                    }
                    ImGui::SameLine();

                    ImGui::TextUnformatted( ICON_MDI_MAGNIFY );
                    ImGui::SameLine();

                    // Name filter — substring match against filenames (see BuildDisplayOrder).
                    ImGui::SetNextItemWidth( 180.0f );
                    ImGui::InputTextWithHint( "##AssetSearch", "Filter by name...", m_SearchBuf,
                                              sizeof( m_SearchBuf ) );
                    ImGui::SameLine();

                    // Sort mode + ascending/descending toggle.
                    ImGui::SetNextItemWidth( 130.0f );
                    const char* const sortNames[] = { "Name", "Date Modified", "Type", "Size" };
                    int               sortIdx     = static_cast<int>( m_SortMode );
                    if ( ImGui::Combo( "##AssetSort", &sortIdx, sortNames, IM_ARRAYSIZE( sortNames ) ) )
                        m_SortMode = static_cast<SortMode>( sortIdx );
                    ImGui::SameLine();
                    if ( ImGui::Button( m_SortDescending ? ICON_MDI_SORT_DESCENDING : ICON_MDI_SORT_ASCENDING ) )
                        m_SortDescending = !m_SortDescending;
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( m_SortDescending ? "Descending" : "Ascending" );
                    ImGui::SameLine();

                    // Type filter — show only one asset kind (folders always stay visible).
                    ImGui::SetNextItemWidth( 130.0f );
                    static const struct
                    {
                        const char* Label;
                        int         Type;
                    } kTypeFilters[] = {
                         { "All Types", -1 },
                         { "Scenes", static_cast<int>( FileType::Scene ) },
                         { "Prefabs", static_cast<int>( FileType::Prefab ) },
                         { "Scripts", static_cast<int>( FileType::Script ) },
                         { "Textures", static_cast<int>( FileType::Texture ) },
                         { "Materials", static_cast<int>( FileType::Material ) },
                         { "Models", static_cast<int>( FileType::Model ) },
                         { "Shader Graphs", static_cast<int>( FileType::ShaderGraph ) },
                         { "Audio", static_cast<int>( FileType::Audio ) },
                         { "Clouds", static_cast<int>( FileType::Cloud ) },
                         { "Skeletal Meshes", static_cast<int>( FileType::SkinnedMesh ) },
                         { "Skeletons", static_cast<int>( FileType::Skeleton ) },
                         { "Animations", static_cast<int>( FileType::Animation ) },
                         { "Foliage Types", static_cast<int>( FileType::FoliageType ) },
                         { "Level Sequences", static_cast<int>( FileType::LevelSequence ) },
                         { "VFX Systems", static_cast<int>( FileType::VFXSystem ) },
                         { "Fractures", static_cast<int>( FileType::Fracture ) },
                         { "Input Actions", static_cast<int>( FileType::InputAction ) },
                         { "Input Mapping Contexts", static_cast<int>( FileType::InputMappingContext ) },
                    };
                    const char* currentFilter = "All Types";
                    for ( const auto& f : kTypeFilters )
                        if ( f.Type == m_TypeFilter )
                            currentFilter = f.Label;
                    if ( ImGui::BeginCombo( "##AssetTypeFilter", currentFilter ) )
                    {
                        for ( const auto& f : kTypeFilters )
                            if ( ImGui::Selectable( f.Label, f.Type == m_TypeFilter ) )
                                m_TypeFilter = f.Type;
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();

                    // Back / Forward / Up navigation.
                    ImGui::BeginDisabled( m_NavPos <= 0 );
                    if ( ImGui::Button( ICON_MDI_ARROW_LEFT ) )
                        GoBack();
                    ImGui::EndDisabled();
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Back" );
                    ImGui::SameLine();

                    ImGui::BeginDisabled( m_NavPos + 1 >= static_cast<int>( m_NavHistory.size() ) );
                    if ( ImGui::Button( ICON_MDI_ARROW_RIGHT ) )
                        GoForward();
                    ImGui::EndDisabled();
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Forward" );
                    ImGui::SameLine();

                    ImGui::BeginDisabled( !m_CurrentDir || m_CurrentDir == m_BaseProjectDir );
                    if ( ImGui::Button( ICON_MDI_ARROW_UP_BOLD ) && m_CurrentDir )
                        ChangeDirectory( m_CurrentDir->Parent );
                    ImGui::EndDisabled();
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Up" );
                    ImGui::SameLine();

                    // Favorites: jump to a pinned folder (added via a folder's right-click menu).
                    if ( ImGui::Button( ICON_MDI_STAR ) )
                        ImGui::OpenPopup( "##favMenu" );
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Favorite folders" );
                    if ( ImGui::BeginPopup( "##favMenu" ) )
                    {
                        const std::vector<std::string> favourites = EditorPreferences::CurrentFavouriteFolders();
                        if ( favourites.empty() )
                            ImGui::TextDisabled( "No favorites — right-click a folder -> Add to Favorites." );
                        for ( const auto& fav : favourites )
                        {
                            const std::string label = std::filesystem::path( fav ).filename().string();
                            if ( ImGui::MenuItem( ( label.empty() ? fav : label ).c_str() ) )
                                NavigateToPath( fav );
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine();
                    if ( ImGui::Button( ICON_MDI_FILE_IMPORT " Import" ) )
                    {
                        ImportExternalTexture();
                    }
                    ImGui::SameLine();

                    if ( m_UpdateNavigationPath )
                    {
                        m_BreadCrumbData.clear();
                        auto current = m_CurrentDir;
                        while ( current )
                        {
                            if ( current->Parent != nullptr )
                            {
                                m_BreadCrumbData.push_back( current );
                                current = current->Parent;
                            }
                            else
                            {
                                m_BreadCrumbData.push_back( m_BaseProjectDir );
                                current = nullptr;
                            }
                        }

                        for ( size_t i = 0; i < m_BreadCrumbData.size() / 2; i++ )
                        {
                            std::swap( m_BreadCrumbData[i], m_BreadCrumbData[m_BreadCrumbData.size() - i - 1] );
                        }

                        m_UpdateNavigationPath = false;
                    }
                    {
                        int newPwdLastSecIdx = -1;
                        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.1f, 0.2f, 0.7f, 0.0f ) );

                        for ( size_t i = 0; i < m_BreadCrumbData.size(); ++i )
                        {
                            auto*       directory = m_BreadCrumbData[i];
                            std::string fileName =
                                 std::filesystem::path( directory->AssetPath ).filename().string();

                            ImGui::PushID( directory );
                            if ( ImGui::SmallButton( fileName.c_str() ) )
                                ChangeDirectory( directory );
                            ImGui::PopID();
                            ImGui::SameLine();

                            if ( i + 1 < m_BreadCrumbData.size() )
                            {
                                ImGui::TextDisabled( ">" );
                                ImGui::SameLine();
                            }
                        }
                        ImGui::PopStyleColor();

                        if ( newPwdLastSecIdx >= 0 )
                        {
                            // Implementation for path navigation
                        }

                        ImGui::SameLine();
                    }
                    ImGui::EndChild();
                }

                {
                    // The grid takes the whole body: asset details live in the hover tooltip
                    // (DrawAssetTooltip), not in a strip that a selection carves out of the panel.
                    ImGui::BeginChild( "##assetBodyRegion", ImVec2( 0.0f, 0.0f ), false );

                    int shownIndex = 0;

                    float xAvail = ImGui::GetContentRegionAvail().x;

                    constexpr float padding              = 4.0f;
                    const float     scaledThumbnailSize  = m_GridSize; // * ImGui::GetIO().FontGlobalScale;
                    const float     scaledThumbnailSizeX = scaledThumbnailSize * 0.55f;
                    // Column stride MUST match the cell RenderFile actually draws (m_GridSize wide: centered
                    // icon + wrapped label + the ~6px card padding). The old value used the thumbnail-only
                    // 0.55*grid width, which packed ~1.6x too many columns -> cards overlapped and labels
                    // shifted into the next column.
                    // Card outsets in RenderFile (±2px horizontal, 8px above / 6px below the content) plus
                    // breathing room so neighbouring cards and their shadow tiles never touch.
                    const float cardPadX = 8.0f;
                    const float cardPadY = 14.0f;
                    const float cellSize = m_GridSize + 4.0f + cardPadX * 2.0f + ImGui::GetStyle().ItemSpacing.x;

                    const ImVec2 backgroundThumbnailSize = { scaledThumbnailSizeX + padding * 2,
                                                             scaledThumbnailSize + padding * 2 };

                    const float panelWidth  = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
                    int         columnCount = static_cast<int>( panelWidth / cellSize );
                    if ( columnCount < 1 )
                        columnCount = 1;

                    int flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

                    if ( m_IsInListView )
                    {
                        ImGui::PushStyleVar( ImGuiStyleVar_CellPadding, { 0, 0 } );
                        columnCount = 1;
                        flags |= ImGuiTableFlags_RowBg | ImGuiTableFlags_NoPadOuterX |
                                 ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_SizingStretchSame;
                    }
                    else
                    {
                        ImGui::PushStyleVar( ImGuiStyleVar_CellPadding, { cardPadX, cardPadY } );
                        flags |= ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit;
                    }

                    ImVec2       cursorPos = ImGui::GetCursorPos();
                    const ImVec2 region    = ImGui::GetContentRegionAvail();
                    // Skip the drop-target background when the body has no area (a zero size asserts inside
                    // InvisibleButton) — there is nothing to drop onto in a collapsed/zero-size panel.
                    if ( region.x > 0.0f && region.y > 0.0f )
                        ImGui::InvisibleButton( "##DragDropTargetAssetPanelBody", region );

                    ImGui::SetCursorPos( cursorPos );

                    // Tile-hover tracking for the empty-click deselect below: the ScrollY table is its own
                    // child window, so a backdrop item in THIS window never sees hover — RenderFile flags
                    // hovered tiles instead.
                    m_TileHovered = false;

                    if ( ImGui::BeginTable( "BodyTable", columnCount, flags ) )
                    {
                        // Grid: pin every column to the card's real width. SizingFixedFit alone sizes a
                        // column to its CONTENT (icon/label), which can be narrower than the m_GridSize-wide
                        // card RenderFile paints — neighbouring cards then overlapped horizontally.
                        if ( !m_IsInListView )
                            for ( int ci = 0; ci < columnCount; ++ci )
                                ImGui::TableSetupColumn( nullptr, ImGuiTableColumnFlags_WidthFixed,
                                                         m_GridSize + 4.0f );

                        m_GridItemsPerRow =
                             (int)floor( xAvail / ( m_GridSize + ImGui::GetStyle().ItemSpacing.x ) );
                        m_GridItemsPerRow = std::max( 1, m_GridItemsPerRow );

                        // ImGuiUtilities::PushID();

                        // Filtered (search) + sorted (name/date/type/size) display order; both views share it.
                        const std::vector<size_t> displayOrder = BuildDisplayOrder();
                        for ( size_t idx : displayOrder )
                        {
                            ImGui::TableNextColumn();
                            // ONLY WHAT IS ON SCREEN IS DRAWN (THUMB3), as UE's tile view only builds the
                            // widgets in view: a tile scrolled away is a Dummy of the last drawn tile's
                            // height — no thumbnail lookup, no capture request, no file probe. The selected
                            // tile is always drawn, so keyboard navigation can scroll to it.
                            float&      cellHeight = m_CellHeight[m_IsInListView ? 1 : 0];
                            const float cellWidth  = ImGui::GetContentRegionAvail().x;
                            if ( cellHeight > 0.0f && !ImGui::IsRectVisible( ImVec2( cellWidth, cellHeight ) ) &&
                                 !IsSelected( m_CurrentDir->Children[idx] ) )
                            {
                                ImGui::Dummy( ImVec2( cellWidth, cellHeight ) );
                                shownIndex++;
                                continue;
                            }
                            const float cellTop = ImGui::GetCursorPosY();
                            const bool  doubleClicked =
                                 RenderFile( static_cast<int>( idx ), !m_CurrentDir->Children[idx]->IsFile,
                                             shownIndex, !m_IsInListView );
                            cellHeight = std::max( cellHeight, ImGui::GetCursorPosY() - cellTop );
                            if ( doubleClicked )
                                break;
                            shownIndex++;
                        }

                        // ImGuiUtilities::PopID();

                        if ( ImGui::BeginPopupContextWindow( "AssetPanelHierarchyContextWindow",
                                                             ImGuiPopupFlags_MouseButtonRight |
                                                                  ImGuiPopupFlags_NoOpenOverItems ) )
                        {
                            if ( !m_Clipboard.empty() &&
                                 ImGui::Selectable( m_ClipboardCut ? "Paste (move)" : "Paste (copy)" ) )
                            {
                                PasteClipboard();
                            }

                            ImGui::Separator();

                            if ( ImGui::Selectable( "Import Texture..." ) )
                            {
                                ImportExternalTexture();
                            }

                            if ( ImGui::Selectable( "Refresh" ) )
                            {
                                QueueRefresh();
                            }

                            if ( ImGui::Selectable( "New folder" ) )
                            {
                                std::string fullPath = m_CurrentDir->AssetPath + "/NewFolder";
                                std::filesystem::create_directory( fullPath );
                                QueueRefresh();
                            }

                            if ( ImGui::Selectable( "New Material" ) )
                                CreateNewMaterial();

                            if ( ImGui::Selectable( std::string( kNewLevelSequenceLabel ).c_str() ) )
                                if ( const auto created = CreateNewLevelSequence(); !created )
                                    LOG_ERROR( "[Content] {}", created.GetError() );

                            if ( ImGui::Selectable( std::string( kNewInputActionLabel ).c_str() ) )
                                if ( const auto created = CreateNewInputAction(); !created )
                                    LOG_ERROR( "[Content] {}", created.GetError() );

                            if ( ImGui::Selectable( std::string( kNewInputMappingContextLabel ).c_str() ) )
                                if ( const auto created = CreateNewInputMappingContext(); !created )
                                    LOG_ERROR( "[Content] {}", created.GetError() );

                            // Pick the domain up front (like Unreal's Material Domain / Godot's Mode):
                            // it decides the output node, vertex contract and palette of the new graph.
                            if ( ImGui::BeginMenu( "New Shader Graph" ) )
                            {
                                auto createGraph = [&]( ShaderGraph::Domain domain )
                                {
                                    const auto path =
                                         NodeGraphPanel::CreateNewGraphFile( m_CurrentDir->AssetPath, domain );
                                    if ( path.empty() ) // not written — nothing to open, nothing new to list
                                        return;
                                    // Through the SAME opener the double-click uses, so a graph created
                                    // here and a graph opened from the tile reach one window by one route.
                                    if ( RequestShaderGraphDocument( m_AssetManager, path ) !=
                                         ShaderGraphDocumentRequest::Requested )
                                    {
                                        LOG_ERROR( "[ShaderGraph] '{}' was created but would not open.", path );
                                    }
                                    QueueRefresh();
                                };
                                if ( ImGui::MenuItem( "Surface" ) )
                                    createGraph( ShaderGraph::Domain::Surface );
                                if ( ImGui::MenuItem( "Post Process" ) )
                                    createGraph( ShaderGraph::Domain::PostProcess );
                                // The cloud medium. Named for what an artist is authoring rather than
                                // for the engine's domain token: "Volume" is the word in the `.shader`
                                // and in ShaderDomain, and it means nothing beside "Surface" and "Post
                                // Process" until you already know what it is.
                                if ( ImGui::MenuItem( "Cloud Medium" ) )
                                    createGraph( ShaderGraph::Domain::Volume );
                                ImGui::EndMenu();
                            }

                            // THE FOUR CLOUD FORMATS. Until this menu existed not one of them could be
                            // created: all four editors are contextual documents keyed on an asset handle,
                            // so the double-click seam had nothing to open and an artist could edit the
                            // twenty-one shipped assets and author none of their own.
                            //
                            // A submenu for the reason "New Shader Graph" is one — four more top-level
                            // items would be half the menu.
                            if ( ImGui::BeginMenu( "New Cloud Asset" ) )
                            {
                                // Disabled while a volume is being generated: only one creation is tracked
                                // at a time, and a second click would detach the first bake's thread.
                                ImGui::BeginDisabled( m_CloudBakeRunning );

                                if ( ImGui::MenuItem( "Cloud Type" ) )
                                    CreateNewCloudAsset( CloudAssetKind::Type );
                                if ( ImGui::IsItemHovered() )
                                    ImGui::SetTooltip( "A kind of cloud: altitudes, silhouette curve, "
                                                       "density. Starts from the built-in congestus." );

                                if ( ImGui::MenuItem( "Cloud Layout" ) )
                                    CreateNewCloudAsset( CloudAssetKind::Layout );
                                if ( ImGui::IsItemHovered() )
                                    ImGui::SetTooltip( "A blank 512x512 painting of where clouds are. "
                                                       "Draw on it in the layout document." );

                                if ( ImGui::MenuItem( "Cloud Noise Volume" ) )
                                    CreateNewCloudAsset( CloudAssetKind::NoiseVolume );
                                if ( ImGui::IsItemHovered() )
                                    ImGui::SetTooltip( "The 3D noise cloud edges are eroded with, 128^3 "
                                                       "RGBA8. Generated in the background - it takes "
                                                       "several seconds and a progress bar appears above." );

                                if ( ImGui::MenuItem( "Cloud Modelling Volume" ) )
                                    CreateNewCloudAsset( CloudAssetKind::ModellingVolume );
                                if ( ImGui::IsItemHovered() )
                                    ImGui::SetTooltip( "A hero cloud's sculpted body, 128x64x128. Starts "
                                                       "from the shipped congestus and is baked in the "
                                                       "background." );

                                ImGui::EndDisabled();
                                ImGui::EndMenu();
                            }

                            if ( !m_IsInListView )
                            {
                                ImGui::SliderFloat( "##GridSize", &m_GridSize, m_MinGridSize, m_MaxGridSize );
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();

                    // Left-click anywhere in the body that is NOT over a tile clears the selection (and
                    // folds the preview pane). Hover is checked window-wide including the table's child.
                    if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !m_TileHovered &&
                         ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows ) )
                    {
                        m_Selection.clear();
                        m_CurrentSelected = nullptr;
                    }

                    ImGui::EndChild();
                }
            }
            ImGui::EndChild(); // ##cb_right

            if ( ImGui::BeginDragDropTarget() )
            {
                auto data =
                     ImGui::AcceptDragDropPayload( "selectable", ImGuiDragDropFlags_AcceptNoDrawDefaultRect );
                if ( data )
                {
                    std::string* a = (std::string*)data->Data;
                    if ( MoveFileTo( *a, m_MovePath ) )
                    {
                        // LINFO("Moved File: %s to %s", a->c_str(), m_MovePath.c_str());
                    }
                    m_IsDragging = false;
                }
                ImGui::EndDragDropTarget();
>>>>>>> origin/task/GP2
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

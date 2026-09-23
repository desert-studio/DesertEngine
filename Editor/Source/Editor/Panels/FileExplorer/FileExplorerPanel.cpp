#define IMGUI_DEFINE_MATH_OPERATORS

// This TU now pulls engine headers (via UIHelper -> Engine/Desert.hpp) that use std::max/std::min and
// std::numeric_limits<>::max(); keep the windows.h min/max macros from clobbering them.
#define NOMINMAX

#include "FileExplorerPanel.hpp"
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/SceneOpenRequest.hpp>
#include <Editor/Panels/Clouds/CloudDocumentOpen.hpp>
#include <Editor/Panels/FileExplorer/NewCloudAsset.hpp>
#include <Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp>
#include <Editor/Core/AssetFileOps.hpp>
#include <Editor/Core/AssetReferences.hpp>
#include <Editor/Core/EditorPreferences.hpp> // the pinned folders live in editor.json (К5)
#include <Editor/Panels/NodeGraph/NodeGraphPanel.hpp>
#include <Editor/Panels/NodeGraph/ShaderGraphDocumentOpen.hpp>
#include "../../Core/EditorResources.hpp"

#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/MeshMaterial.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Editor/Widgets/ThumbnailSweep.hpp>
#include <Engine/Assets/AssetManager.hpp>
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
#include <Engine/Core/Scene.hpp>            // GetFinalImage (Capture Thumbnail from viewport)
#include <Engine/Graphic/Renderer.hpp>      // WaitDeviceIdle before readback
#include <Engine/Graphic/Image.hpp>         // Image2D::ReadPixelsRGBA8
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
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef DESERT_PLATFORM_WINDOWS
    #include <windows.h>
    #include <shellapi.h>
#endif

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // Cross-platform move into a destination DIRECTORY (std::filesystem, with a copy+remove fallback
        // across volumes). Replaces the old Windows-only `system("move ...")` that no-op'd on macOS.
        bool MoveFile( const std::string& filePath, const std::string& movePath )
        {
            std::string newPath, error;
            return AssetFileOps::Move( filePath, movePath, newPath, error );
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
         { FileType::Unknown, "Unknown" },   { FileType::Scene, "Scene" },
         { FileType::Prefab, "Prefab" },     { FileType::Script, "Script" },
         { FileType::Shader, "Shader" },     { FileType::Texture, "Texture" },
         { FileType::Font, "Font" },         { FileType::Cubemap, "Cubemap" },
         { FileType::Model, "Model" },       { FileType::Audio, "Audio" },
         { FileType::Material, "Material" }, { FileType::ShaderGraph, "Shader Graph" },
         { FileType::Cloud, "Cloud" },
    };

    static const std::unordered_map<std::string, FileType> s_FileTypes = {
         { "lsn", FileType::Scene },
         { "deprefab", FileType::Prefab },
         { "prefab", FileType::Prefab },
         { "lprefab", FileType::Prefab },
         { "cs", FileType::Script },
         { "lua", FileType::Script },
         { "glsl", FileType::Shader },
         { "shader", FileType::Shader },
         { "frag", FileType::Shader },
         { "vert", FileType::Shader },
         { "comp", FileType::Shader },
         { "png", FileType::Texture },
         { "jpg", FileType::Texture },
         { "jpeg", FileType::Texture },
         { "bmp", FileType::Texture },
         { "gif", FileType::Texture },
         { "tga", FileType::Texture },
         { "ttf", FileType::Font },
         { "hdr", FileType::Cubemap },
         { "obj", FileType::Model },
         { "fbx", FileType::Model },
         { "gltf", FileType::Model },
         { "glb", FileType::Model },
         { "blend", FileType::Model },
         { "mp3", FileType::Audio },
         { "m4a", FileType::Audio },
         { "wav", FileType::Audio },
         { "ogg", FileType::Audio },
         { "lmat", FileType::Material },
         // Engine-native extensions (see Common::Constants::Extensions).
         { "demat", FileType::Material },
         { "desce", FileType::Scene },
         { "demesh", FileType::Model },
         { "dgraph", FileType::ShaderGraph },
         // `.ini` WAS TYPED BY NOBODY AND READ BY SOMEBODY. `DrawPreviewPane` has always asked
         // `entry->Type == FileType::Ini` to decide whether to show a text excerpt, and no key in this
         // map ever produced that value — a condition that could not be true, guarding a feature that
         // therefore did not exist. One line makes the reader's intent real: an ini IS text, and the
         // excerpt is what a person wants from one.
         { "ini", FileType::Ini },
         // The four cloud formats. Typed here for the first time in M11 — they used to fall through to
         // FileType::Unknown, which is why they had one grey glyph between them, no colour, no entry in
         // the type filter and no thumbnail. THIS MAP IS THE CENSUS'S SUBJECT: every key in it must have
         // a row in Editor/Widgets/ThumbnailFormats.hpp saying who makes its picture or why nobody does,
         // and Desert/Tests/Editor/ThumbnailFormats reads this literal to check it.
         { "dclayout", FileType::Cloud },
         { "dcnv", FileType::Cloud },
         { "dcmv", FileType::Cloud },
         { "decloudtype", FileType::Cloud },
         // The UI theme (Ю13). Typed here so it gets a colour, an icon and a row in the browser's type
         // filter — and so the census above applies to it: it needs a row in ThumbnailFormats.hpp saying
         // who makes its picture or why nobody does.
         { "detheme", FileType::UITheme },
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
    };

    FileExplorerPanel::FileExplorerPanel( const std::filesystem::path&         rootPath,
                                          const SubjectEditorRegistry*         subjectEditors,
                                          Assets::AssetManager*                assetManager,
                                          std::weak_ptr<::Desert::Core::Scene> viewportScene )
         // IN DECLARATION ORDER. Members are constructed in the order they are DECLARED whatever this list
         // says, so a list in a different order is a reader being told the wrong sequence — harmless here
         // because every initialiser is a literal or a parameter, and exactly how a later initialiser that
         // reads an earlier member becomes a use-before-init nobody can see.
         : IPanel( "Assets" ), m_CurrentPath( rootPath ), m_MinGridSize( 40.0f ), m_MaxGridSize( 400.0f ),
           m_IsDragging( false ), m_IsInListView( false ), m_ShowHiddenFiles( false ), m_GridSize( 120.0f ),
           m_Refresh( false ), m_UpdateNavigationPath( true ), m_CurrentDir( nullptr ),
           m_BaseProjectDir( nullptr ), m_PreviousDirectory( nullptr ), m_AssetManager( assetManager ),
           m_SubjectEditors( subjectEditors ), m_ViewportScene( std::move( viewportScene ) )
    {
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_Thumbnails = std::make_unique<ThumbnailCache>();
        m_Sweeper    = std::make_unique<ThumbnailSweeper>();
        ThumbnailCache::PurgeOldVersions(); // drop stale-renderer thumbnails so they regenerate cleanly

        // NO LOAD STEP FOR THE PINNED FOLDERS ANY MORE (К5). They are read out of EditorPreferences where
        // they are drawn; a panel-local copy taken at construction is the shape К6 removed from the gizmo
        // snap, and here it would additionally be a copy of a list the user can change while the panel is
        // alive (opening a different project changes which pins exist).

#ifdef DESERT_PLATFORM_WINDOWS
        m_Delimiter = std::string( "\\" );
#else
        m_Delimiter = std::string( "/" );
#endif
        float dpi  = 1.0;
        m_GridSize = 120.0f;
        m_GridSize *= dpi;
        m_MinGridSize = 40.0f;
        m_MaxGridSize = 400.0f;
        m_MinGridSize *= dpi;
        m_MaxGridSize *= dpi;
        m_BasePath = m_CurrentPath.string();

#ifdef DESERT_PLATFORM_WINDOWS
        m_Delimiter = "\\";
#else
        m_Delimiter = "/";
#endif

        if ( rootPath.empty() )
        {
            m_BasePath = "Assets"; 
        }
        else
        {
            m_BasePath = rootPath.string();
        }

        std::string baseDirectoryHandle = ProcessDirectory( m_BasePath, nullptr, true );

        if ( m_Directories.find( baseDirectoryHandle ) != m_Directories.end() )
        {
            m_BaseProjectDir = m_Directories[baseDirectoryHandle].get();
            m_CurrentDir     = m_BaseProjectDir;

            if ( m_BaseProjectDir )
            {
                ChangeDirectory( m_BaseProjectDir );
            }
        }
    }

    FileExplorerPanel::~FileExplorerPanel()
    {
        // CANCEL, THEN WAIT. The worker writes into m_CloudBakeProgress and into a path this object owns,
        // so the panel must outlive it; waiting rather than detaching is the difference between a slow
        // editor shutdown and a use-after-free. The same discipline CloudModellingVolumePanel keeps, for
        // the same reason.
        //
        // The cancel bounds the wait for a `.dcmv` (the bake asks the callback whether to carry on between
        // slabs). A `.dcnv` cannot be cancelled — Assets::GenerateCloudNoiseVolume takes a progress
        // pointer and no stop condition — so closing the editor during one waits out the remainder of a
        // bake measured at 8.7 s in Debug. That is the existing CloudNoiseVolumePanel's behaviour too, and
        // fixing it means a stop hook on an engine function this task does not own.
        m_CloudBakeCancelled.store( true );
        if ( m_CloudBake.valid() )
            m_CloudBake.wait();
    }

    namespace
    {
        // Cheap signature of a directory's immediate entries (name + write-time). Detects external
        // add/remove/rename without an OS watch API.
        size_t DirectorySignature( const std::string& dirPath )
        {
            size_t          sig = 0;
            std::error_code ec;
            if ( !std::filesystem::is_directory( dirPath, ec ) )
                return sig;
            for ( const auto& entry : std::filesystem::directory_iterator( dirPath, ec ) )
            {
                if ( ec )
                    break;
                sig ^= std::hash<std::string>{}( entry.path().filename().string() ) + 0x9e3779b9 + ( sig << 6 ) +
                       ( sig >> 2 );
                std::error_code tec;
                const auto      t = std::filesystem::last_write_time( entry.path(), tec );
                if ( !tec )
                    sig ^= static_cast<size_t>( t.time_since_epoch().count() ) + 0x9e3779b9 + ( sig << 6 ) +
                           ( sig >> 2 );
            }
            return sig;
        }
    } // namespace

    void FileExplorerPanel::OnPreUpdate()
    {
        // BEFORE the throttle and before the early return on a null directory: a generation that has
        // finished must be collected on the frame it finished, whatever the browser is looking at. A
        // future nobody polls is a thread whose result is thrown away at shutdown.
        PollCloudAssetBake();

        // THE BACKGROUND SWEEP, ahead of the throttle below and unconditional. It has a period of its
        // own (ThumbnailSweeper::kFramesBetweenScans) and paces itself; putting it behind this panel's
        // directory-poll throttle would make one background rate depend on another for no reason, and
        // putting it after the `!m_CurrentDir` return would stop the whole project's previews the moment
        // the browser had no directory selected.
        if ( m_Sweeper )
            m_Sweeper->Tick( m_AssetManager, m_BasePath );

        // Throttle to ~every 30 frames (~0.5s @60fps) — directory_iterator is cheap but not free.
        if ( ++m_PollCounter < 30 )
            return;
        m_PollCounter = 0;

        if ( !m_CurrentDir )
            return;

        const size_t sig = DirectorySignature( m_CurrentDir->AssetPath );
        if ( sig != m_DirSignature )
        {
            m_DirSignature = sig;
            QueueRefresh();
        }
    }

    void FileExplorerPanel::ChangeDirectory( DirectoryInformation* directory )
    {
        if ( !directory )
            return;

        m_PreviousDirectory    = m_CurrentDir;
        m_CurrentDir           = directory;
        m_UpdateNavigationPath = true;

        if ( !m_CurrentDir->Opened )
        {
            ProcessDirectory( m_CurrentDir->AssetPath, m_CurrentDir->Parent, true );
        }

        // Record in the back/forward history, unless this navigation IS a back/forward.
        if ( !m_NavigatingHistory )
        {
            if ( m_NavPos + 1 < static_cast<int>( m_NavHistory.size() ) )
                m_NavHistory.resize( m_NavPos + 1 ); // drop the forward branch
            if ( m_NavHistory.empty() || m_NavHistory.back() != directory->AssetPath )
            {
                m_NavHistory.push_back( directory->AssetPath );
                m_NavPos = static_cast<int>( m_NavHistory.size() ) - 1;
            }
        }
    }

    void FileExplorerPanel::NavigateToPath( const std::string& path )
    {
        if ( auto it = m_Directories.find( path ); it != m_Directories.end() )
        {
            ChangeDirectory( it->second.get() );
            return;
        }

        // Not loaded yet (e.g. a favorite from a previous session): expand the tree from the project
        // root down to `path`, matching one segment at a time.
        if ( !m_BaseProjectDir )
            return;
        const std::filesystem::path base = m_BaseProjectDir->AssetPath;
        std::error_code             ec;
        const std::filesystem::path rel = std::filesystem::relative( path, base, ec );
        // "Not under the project" = the relative path starts with a ".." component. Compare path
        // ELEMENTS rather than the native string: path::native() is std::wstring on Windows, so a
        // narrow ".." literal does not even overload-resolve there.
        if ( ec || rel.empty() || *rel.begin() == std::filesystem::path( ".." ) )
            return; // not under the project

        DirectoryInformation* cur = m_BaseProjectDir;
        if ( !cur->Opened )
            ProcessDirectory( cur->AssetPath, cur->Parent, true );
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
                return; // path no longer exists
            if ( !next->Opened )
                ProcessDirectory( next->AssetPath, next->Parent, true );
            cur = next;
        }
        ChangeDirectory( cur );
    }

    void FileExplorerPanel::GoBack()
    {
        if ( m_NavPos <= 0 )
            return;
        --m_NavPos;
        m_NavigatingHistory = true;
        NavigateToPath( m_NavHistory[m_NavPos] );
        m_NavigatingHistory = false;
    }

    void FileExplorerPanel::GoForward()
    {
        if ( m_NavPos + 1 >= static_cast<int>( m_NavHistory.size() ) )
            return;
        ++m_NavPos;
        m_NavigatingHistory = true;
        NavigateToPath( m_NavHistory[m_NavPos] );
        m_NavigatingHistory = false;
    }

    void FileExplorerPanel::AddPrefabToScene( const std::string& prefabPath )
    {
        auto scene = m_ViewportScene.lock();
        if ( !scene || !m_AssetManager )
            return;

        auto prefab = m_AssetManager->FindByPath<Assets::PrefabAsset>( prefabPath );
        if ( !prefab )
            prefab = m_AssetManager->CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High,
                                                                       prefabPath );
        if ( !prefab )
            return;
        if ( !prefab->IsReadyForUse() )
            prefab->Load();

        // Same instantiate path the viewport-drop uses (undoable, selects the new root). Under the
        // SELECTED entity when there is one, so a UI prefab inserted from here lands inside the canvas
        // the author is working in rather than at the scene root, where it would draw nothing.
        ECS::Entity parentEntity;
        if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
        {
            if ( auto ref = scene->FindEntityByID( *sel ) )
            {
                parentEntity = ref->get();
            }
        }

        const auto placed = prefab->Instantiate( scene.get(), *m_AssetManager, parentEntity, nullptr );
        if ( !placed )
        {
            LOG_ERROR( "{}", placed.GetError() );
            return;
        }
        const ECS::Entity root = placed.GetValue();
        if ( root )
        {
            const auto uuid = root.GetComponent<ECS::UUIDComponent>().UUID;
            Core::SelectionManager::SetSelected( uuid );
            Commands::NotifyCreated( { uuid } );
        }
    }

    void FileExplorerPanel::CreateNewMaterial()
    {
        if ( !m_CurrentDir )
            return;
        const std::string ext( Common::Constants::Extensions::MATERIAL_EXTENSION );
        const std::string name = AssetFileOps::UniqueName(
             "NewMaterial", ext,
             [&]( const std::string& n )
             { return std::filesystem::exists( std::filesystem::path( m_CurrentDir->AssetPath ) / n ); } );
        const auto path = std::filesystem::path( m_CurrentDir->AssetPath ) / name;
        // Minimal valid material: no params, no textures — the engine derives a stable id from the path.
        // The refresh below is what puts the new material in front of the user, so it runs only when
        // there is a file to show: a refresh over a failed write just redraws the old listing and the
        // user is left believing the "New Material" menu item did nothing at all.
        if ( const auto written =
                  Common::Utils::FileSystem::WriteContentToFileAtomic( path, "{\"Params\":[],\"Textures\":[]}" );
             !written )
        {
            LOG_ERROR( "[Content] '{}' was not created: {}", path.generic_string(), written.GetError() );
            return;
        }
        QueueRefresh();
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

    void FileExplorerPanel::RemoveDirectory( DirectoryInformation* directory, bool removeFromParent )
    {
        if ( directory->Parent && removeFromParent )
        {
            directory->Parent->Children.clear();
        }

        for ( auto& subdir : directory->Children )
            RemoveDirectory( subdir, false );

        m_Directories.erase( directory->AssetPath );
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
            auto        fileType   = FileType::Unknown;
            const auto& fileTypeIt = s_FileTypes.find( extension );
            if ( fileTypeIt != s_FileTypes.end() )
                fileType = fileTypeIt->second;

            directoryInfo->IsFile = true;
            directoryInfo->Type   = fileType;
            directoryInfo->FileSize =
                 std::filesystem::exists( stdPath ) ? std::filesystem::file_size( stdPath ) : 0;
            {
                std::error_code wec;
                const auto      t = std::filesystem::last_write_time( stdPath, wec );
                directoryInfo->LastWriteTime =
                     wec ? 0 : static_cast<uint64_t>( t.time_since_epoch().count() );
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
        // Captured for the OS file-drop handler (which runs outside the ImGui frame, via OnEvent).
        m_IsHovered = ImGui::IsWindowHovered( ImGuiHoveredFlags_RootAndChildWindows );

        // Keyboard shortcuts on the selected item (panel focused, no text field active): F2 rename, Del delete.
        if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) && m_CurrentSelected &&
             !ImGui::GetIO().WantTextInput )
        {
            if ( ImGui::IsKeyPressed( ImGuiKey_F2, false ) )
            {
                m_RenamePath = m_CurrentSelected->AssetPath;
                std::snprintf(
                     m_RenameBuf, sizeof( m_RenameBuf ), "%s",
                     std::filesystem::path( m_CurrentSelected->AssetPath ).filename().string().c_str() );
                m_ShowRenamePopup = true;
            }
            if ( ImGui::IsKeyPressed( ImGuiKey_Delete, false ) )
            {
                m_PendingDeleteList = SelectionPaths();
                m_DeleteReferencers.clear();
                m_ShowDeleteConfirm = true;
            }
            // Clipboard: Ctrl/Cmd + C / X / V.
            const bool mod = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
            if ( mod && ImGui::IsKeyPressed( ImGuiKey_C, false ) )
            {
                m_Clipboard    = SelectionPaths();
                m_ClipboardCut = false;
            }
            if ( mod && ImGui::IsKeyPressed( ImGuiKey_X, false ) )
            {
                m_Clipboard    = SelectionPaths();
                m_ClipboardCut = true;
            }
            if ( mod && ImGui::IsKeyPressed( ImGuiKey_V, false ) )
                PasteClipboard();
        }

        // File-op modals (rename / delete) + last error line.
        DrawFileOpsPopups();
        if ( !m_FileOpStatus.empty() )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", m_FileOpStatus.c_str() );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x##clearFileOp" ) )
                m_FileOpStatus.clear();
        }
        DrawCloudAssetBakeStatus();

        // Advance the material-thumbnail capture state machine once per frame (renders + reads back the
        // pending material; see AssetThumbnailRenderer).
        // Thumbnail capture is driven editor-wide by EditorLayer via ThumbnailService.
        {
            FileIndex              = 0;
            if ( m_Refresh )
            {
                RefreshCurrentDirectory(); // in-place: keeps navigation (watcher / import / rebuild)
                m_Refresh = false;
            }

            // ── Content Browser: two panes split by a draggable vertical splitter. LEFT = pinned Favorites
            //    + the project folder tree; RIGHT = toolbar / breadcrumb / asset grid / preview strip. ──
            constexpr float kMinTreeWidth    = 120.0f;
            constexpr float kMinContentWidth = 220.0f;
            constexpr float kSplitterW       = 6.0f;
            const float     totalAvail       = ImGui::GetContentRegionAvail().x;
            m_TreeWidth = std::clamp( m_TreeWidth, kMinTreeWidth,
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
                    MoveFile( *file, m_MovePath );
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
                const ImVec2 mn  = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
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
                    ImGui::BeginChild( "##cb_toolbar",
                                       ImVec2( 0.0f, ImGui::GetFrameHeightWithSpacing() * 2.0f ), false,
                                       ImGuiWindowFlags_NoScrollbar );

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
                    int               sortIdx      = static_cast<int>( m_SortMode );
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
                    // Reserve a bottom strip for the asset preview pane when a file is selected.
                    const float previewH =
                         ( m_CurrentSelected && m_CurrentSelected->IsFile ) ? 175.0f : 0.0f;
                    ImGui::BeginChild( "##assetBodyRegion", ImVec2( 0.0f, -previewH ), false );

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
                            const bool doubleClicked = RenderFile(
                                 (int)idx, !m_CurrentDir->Children[idx]->IsFile, shownIndex, !m_IsInListView );
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

                            // Pick the domain up front (like Unreal's Material Domain / Godot's Mode):
                            // it decides the output node, vertex contract and palette of the new graph.
                            if ( ImGui::BeginMenu( "New Shader Graph" ) )
                            {
                                auto createGraph = [&]( ShaderGraph::Domain domain )
                                {
                                    const auto path = NodeGraphPanel::CreateNewGraphFile(
                                         m_CurrentDir->AssetPath, domain );
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

                    if ( previewH > 0.0f )
                        DrawPreviewPane();
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
                    if ( MoveFile( *a, m_MovePath ) )
                    {
                        // LINFO("Moved File: %s to %s", a->c_str(), m_MovePath.c_str());
                    }
                    m_IsDragging = false;
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    static const char* IconForType( FileType type )
    {
        const auto it = s_FileTypesToIcon.find( type );
        return it != s_FileTypesToIcon.end() ? it->second : ICON_MDI_FILE;
    }

    // Emit the drag-drop payloads a dragged asset can be dropped as. Target widgets accept exactly the
    // type they expect (texture slots: TEXTURE_ASSET; material slots: MATERIAL_ASSET; hierarchy: PREFAB_FILE).
    void FileExplorerPanel::EmitAssetDragSource( const DirectoryInformation& entry )
    {
        if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceAllowNullID ) )
        {
            const std::string& assetPath = entry.AssetPath;
            const char*         type      = "AssetFile";
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
                    img = m_Thumbnails->Get( ThumbnailKey::DiskPath(
                         CookPaths::CookedMesh( assetPath, ".stmesh" ).generic_string() ) );
                }
            }

            constexpr float previewSize = 48.0f;
            if ( img && m_UIHelper )
                m_UIHelper->Image( img, ImVec2( previewSize, previewSize ) );
            else
            {
                const char*  icon = entry.IsFile ? IconForType( entry.Type ) : ICON_MDI_FOLDER;
                const ImVec4 col =
                     entry.IsFile ? entry.FileTypeColour : ImVec4( 0.95f, 0.82f, 0.42f, 1.0f );
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
        if ( !m_UIHelper || !m_Thumbnails || !m_AssetManager )
            return false;

        // Cache PNG path: <versioned thumbnail dir>/<sanitized source path>.png (persists across restarts).
        const std::string pngPath = ThumbnailKey::DiskPath( entry->AssetPath );

        // Stale if the material was edited after the cached thumbnail was written (regenerate then).
        // Through Editor/Widgets/ThumbnailFreshness.hpp, which is the same rule ThumbnailService::ShouldQueue
        // applies — this used to be a hand-written copy of it, and the two answers disagreed for exactly the
        // assets that needed re-rendering: this panel would not draw them and the service would not queue
        // them, so they showed a colour swatch permanently.
        const bool haveFresh =
             ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( pngPath, entry->AssetPath ) ) ==
             ThumbnailFreshness::Verdict::Show;
        if ( !haveFresh )
            m_Thumbnails->Invalidate( pngPath ); // drop the stale decoded image so the new PNG is reloaded

        // Rendered material-on-sphere preview ready + fresh -> show it. (Only Get() once the file exists so
        // the cache never stores a null for this path.)
        if ( haveFresh )
        {
            if ( auto img = m_Thumbnails->Get( pngPath ) )
            {
                m_UIHelper->ImageButton( "##thumb", img, size );
                return true;
            }
        }

        // Resolve material -> handle (load + register so the offscreen render can use it). Through
        // Editor/Widgets/ThumbnailSubject.hpp, which is the SAME resolution the background sweep uses —
        // the two used to be one copy each, and "which file is photographed" is exactly the question this
        // subsystem has already answered twice and differently once.
        const auto subject = ThumbnailSubject::ResolveMaterial( *m_AssetManager, entry->AssetPath );
        if ( !subject )
            return false;

        auto a = m_AssetManager->FindByPath<Assets::SurfaceMaterialAsset>( entry->AssetPath );
        if ( !a )
            return false;

        // Queue through the editor-wide service: it owns the one renderer, deduplicates against what other
        // panels already asked for, skips anything already on disk and never retries an asset that failed.
        ThumbnailService::Get().RequestMaterial( subject.GetValue().Handle, entry->AssetPath,
                                                 subject.GetValue().How );

        // Until the PNG exists, show the albedo colour as a placeholder swatch.
        const glm::vec3 albedo =
             glm::vec3( a->Data().GetParam( "AlbedoColor", glm::vec4( 0.8f, 0.8f, 0.8f, 1.0f ) ) );
        ImGui::ColorButton( "##matswatch", ImVec4( albedo.r, albedo.g, albedo.b, 1.0f ),
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                                 ImGuiColorEditFlags_NoBorder,
                            size );
        return true;
    }

    bool FileExplorerPanel::DrawRenderedMeshThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper || !m_Thumbnails || !m_AssetManager )
            return false;
        if ( m_FailedThumbs.count( entry->AssetPath ) ) // failed to load before -> icon, no per-frame retry
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
        // The mapping is a pure path computation (CookPaths::CookedMesh — fs::relative, no stat), so
        // hoisting it above the freshness check costs nothing; the `exists()` gate that decides "not cooked
        // -> icon" stays where it was, below, because that one IS a filesystem question.
        const std::string cookedStr = CookPaths::CookedMesh( entry->AssetPath, ".stmesh" ).generic_string();

        const std::string pngPath = ThumbnailKey::DiskPath( cookedStr );

        // Same shared rule as the material grid above (Editor/Widgets/ThumbnailFreshness.hpp).
        const bool haveFresh = ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( pngPath, cookedStr ) ) ==
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

        // Cook lookup, build and the three refusals now live in Editor/Widgets/ThumbnailSubject.hpp, so
        // this tile and the background sweep resolve a mesh the same way. Every one of those refusals is
        // permanent for the session here — an uncooked source, a cooked file that will not build, a mesh
        // with no drawable submeshes — so the blacklist keeps the (logging) retry from happening once per
        // frame, exactly as it did when the code was in this function.
        const auto subject = ThumbnailSubject::ResolveMesh( *m_AssetManager, entry->AssetPath );
        if ( !subject )
        {
            m_FailedThumbs.insert( entry->AssetPath );
            return false;
        }

        ThumbnailService::Get().RequestMesh( subject.GetValue().Handle, subject.GetValue().CookedPath,
                                             subject.GetValue().Material );

        // No swatch for meshes — fall back to the type icon until the PNG is ready.
        return false;
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

    void FileExplorerPanel::ImportExternalTexture()
    {
        const auto picked =
             Common::Utils::FileSystem::OpenFileDialog( "Images\0*.png;*.tga;*.jpg;*.jpeg;*.bmp;*.hdr\0All\0*.*\0" );
        if ( !picked.empty() )
            ImportExternalFile( picked );
    }

    void FileExplorerPanel::ImportExternalFile( const std::filesystem::path& src )
    {
        std::error_code ec;
        if ( !m_AssetManager || src.empty() || !std::filesystem::exists( src, ec ) ||
             std::filesystem::is_directory( src, ec ) )
            return;

        // Copy into the current dir (assets must live under Resources/ so the cook paths stay project-relative).
        const std::filesystem::path texDir = Common::Constants::Path::TEXTUREDIR_PATH;
        std::filesystem::path       destDir =
             m_CurrentDir ? std::filesystem::path( m_CurrentDir->AssetPath ) : texDir;
        if ( !std::filesystem::is_directory( destDir ) )
            destDir = texDir;
        std::filesystem::create_directories( destDir, ec );

        std::filesystem::path dest = destDir / src.filename();
        std::filesystem::copy_file( src, dest, std::filesystem::copy_options::overwrite_existing, ec );
        if ( ec )
        {
            LOG_ERROR( "Import: failed to copy '{}' -> '{}': {}", src.string(), dest.string(), ec.message() );
            return;
        }

        // Textures cook + register immediately (instantly usable / draggable). Other files just appear in
        // the panel (meshes cook on next launch / Rebuild Cooked).
        std::string ext = dest.extension().string();
        if ( !ext.empty() && ext[0] == '.' )
            ext = ext.substr( 1 );
        std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
        const auto     it   = s_FileTypes.find( ext );
        const FileType type = it != s_FileTypes.end() ? it->second : FileType::Unknown;
        if ( type == FileType::Texture || type == FileType::Cubemap )
            TextureDnD::ResolveOrImport( *m_AssetManager, dest.generic_string() );

        QueueRefresh();
    }

    void FileExplorerPanel::OnEvent( Common::Event& e )
    {
        Common::EventManager mgr( e );
        mgr.Notify<Common::EventWindowFileDrop>(
             [this]( Common::EventWindowFileDrop& drop ) -> bool
             {
                 if ( !m_IsHovered ) // only when the drop landed on the Assets panel
                     return false;
                 for ( const auto& path : drop.Paths )
                     ImportExternalFile( path );
                 return true;
             } );
    }

    std::vector<size_t> FileExplorerPanel::BuildDisplayOrder() const
    {
        std::vector<size_t> order;
        if ( !m_CurrentDir )
            return order;
        const auto& children = m_CurrentDir->Children;
        order.reserve( children.size() );

        const std::string search = ToLowerCopy( m_SearchBuf );
        for ( size_t i = 0; i < children.size(); ++i )
        {
            const auto* c = children[i];
            if ( !m_ShowHiddenFiles && c->Hidden )
                continue;
            // Type filter (folders always shown so you can still navigate).
            if ( m_TypeFilter >= 0 && c->IsFile && static_cast<int>( c->Type ) != m_TypeFilter )
                continue;
            if ( !search.empty() )
            {
                const std::string name =
                     ToLowerCopy( std::filesystem::path( c->AssetPath ).filename().string() );
                if ( name.find( search ) == std::string::npos )
                    continue;
            }
            order.push_back( i );
        }

        const SortMode mode = m_SortMode;
        const bool     desc = m_SortDescending;
        std::sort( order.begin(), order.end(),
                   [&]( size_t a, size_t b )
                   {
                       const auto* ca = children[a];
                       const auto* cb = children[b];
                       if ( ca->IsFile != cb->IsFile )
                           return !ca->IsFile; // folders always first, regardless of sort
                       int cmp = 0;
                       switch ( mode )
                       {
                           case SortMode::DateModified:
                               cmp = ( ca->LastWriteTime < cb->LastWriteTime )   ? -1
                                     : ( ca->LastWriteTime > cb->LastWriteTime ) ? 1
                                                                                 : 0;
                               break;
                           case SortMode::Type: cmp = static_cast<int>( ca->Type ) - static_cast<int>( cb->Type ); break;
                           case SortMode::Size:
                               cmp = ( ca->FileSize < cb->FileSize ) ? -1 : ( ca->FileSize > cb->FileSize ) ? 1 : 0;
                               break;
                           case SortMode::Name:
                           default: break;
                       }
                       if ( cmp == 0 ) // Name mode + tiebreak: case-insensitive filename
                       {
                           const auto na = ToLowerCopy( std::filesystem::path( ca->AssetPath ).filename().string() );
                           const auto nb = ToLowerCopy( std::filesystem::path( cb->AssetPath ).filename().string() );
                           cmp           = na.compare( nb );
                       }
                       return desc ? cmp > 0 : cmp < 0;
                   } );
        return order;
    }

    void FileExplorerPanel::DrawItemContextMenu( DirectoryInformation& entry )
    {
        if ( !ImGui::BeginPopupContextItem( "##ItemContext" ) )
            return;

        m_CurrentSelected      = &entry;
        const std::string name = std::filesystem::path( entry.AssetPath ).filename().string();
        ImGui::TextDisabled( "%s", name.c_str() );
        ImGui::Separator();

        if ( entry.IsFile )
        {
            if ( ImGui::MenuItem( "Open" ) ) // default Windows app for this file type
                ShellOpenDefault( entry.AssetPath );
            if ( ImGui::MenuItem( "Show in Explorer" ) ) // reveal with the file selected
                ShellRevealInExplorer( entry.AssetPath );
            if ( ImGui::MenuItem( "Open Containing Folder" ) )
                ShellOpenDefault( std::filesystem::path( entry.AssetPath ).parent_path().string() );

            // Quick "Add to Scene" for prefabs (same instantiate path as dragging into the viewport).
            if ( entry.Type == FileType::Prefab && !m_ViewportScene.expired() && m_AssetManager )
            {
                ImGui::Separator();
                if ( ImGui::MenuItem( "Add to Scene" ) )
                    AddPrefabToScene( entry.AssetPath );
            }

            // UE-style: use the current viewport view as this asset's thumbnail (frame it in the scene first).
            // Only meaningful for assets that show a rendered preview (meshes/materials).
            if ( ( entry.Type == FileType::Model || entry.Type == FileType::Material ) &&
                 !m_ViewportScene.expired() )
            {
                ImGui::Separator();
                if ( ImGui::MenuItem( "Capture Thumbnail (from viewport)" ) )
                    CaptureThumbnailFromViewport( entry.AssetPath );
            }
        }
        else
        {
            if ( ImGui::MenuItem( "Open" ) )
                ChangeDirectory( &entry );
            if ( ImGui::MenuItem( "Show in Explorer" ) )
                ShellRevealInExplorer( entry.AssetPath );
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

        // ---- File operations (cross-platform, selection-aware) ----
        // Right-clicking an item outside the current multi-selection makes it the selection.
        if ( !IsSelected( &entry ) )
        {
            m_Selection.clear();
            m_Selection.insert( entry.AssetPath );
            m_CurrentSelected = &entry;
        }
        const std::vector<std::string> sel   = SelectionPaths();
        const size_t                   count = sel.size();

        ImGui::Separator();
        if ( count <= 1 && ImGui::MenuItem( "Rename", "F2" ) )
        {
            m_RenamePath = entry.AssetPath;
            std::snprintf( m_RenameBuf, sizeof( m_RenameBuf ), "%s", name.c_str() );
            m_ShowRenamePopup = true;
        }
        if ( ImGui::MenuItem( count > 1 ? "Duplicate selection" : "Duplicate" ) )
        {
            for ( const auto& p : sel )
            {
                std::string np, err;
                if ( !AssetFileOps::Duplicate( p, np, err ) )
                    m_FileOpStatus = "Duplicate failed: " + err;
            }
            QueueRefresh();
        }
        if ( ImGui::MenuItem( "Cut", "Ctrl+X" ) )
        {
            m_Clipboard    = sel;
            m_ClipboardCut = true;
        }
        if ( ImGui::MenuItem( "Copy", "Ctrl+C" ) )
        {
            m_Clipboard    = sel;
            m_ClipboardCut = false;
        }
        ImGui::Separator();
        if ( ImGui::MenuItem( count > 1 ? "Delete selection" : "Delete", "Del" ) )
        {
            m_PendingDeleteList = sel;
            m_DeleteReferencers.clear();
            // Safe delete: warn if any file in the selection is still referenced (best-effort text scan).
            AssetReferenceIndex idx;
            BuildProjectAssetReferenceIndex( idx );
            for ( const auto& p : sel )
            {
                std::error_code ec;
                const std::string rel =
                     std::filesystem::relative( p, Common::Constants::Path::ASSETS_PATH, ec ).generic_string();
                for ( const auto& r : idx.ReferencersOf( rel ) )
                    m_DeleteReferencers.push_back( r );
            }
            m_ShowDeleteConfirm = true;
        }

        ImGui::EndPopup();
    }

    void FileExplorerPanel::DrawFileOpsPopups()
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

            if ( ImGui::Button( "Rename", ImVec2( 110.0f, 0.0f ) ) || submit )
            {
                std::string np, err;
                if ( AssetFileOps::Rename( m_RenamePath, m_RenameBuf, np, err ) )
                    QueueRefresh();
                else
                    m_FileOpStatus = "Rename failed: " + err;
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
                ImGui::Text(
                     "Delete \"%s\"?",
                     std::filesystem::path( m_PendingDeleteList.front() ).filename().string().c_str() );
            else
                ImGui::Text( "Delete %zu items?", m_PendingDeleteList.size() );

            if ( !m_DeleteReferencers.empty() )
            {
                ImGui::Spacing();
                ImGui::TextColored( ImVec4( 1.0f, 0.6f, 0.3f, 1.0f ),
                                    "Warning: %zu asset(s) still reference the selection:",
                                    m_DeleteReferencers.size() );
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
                        m_FileOpStatus = "Delete failed: " + err;
                }
                m_Selection.clear();
                QueueRefresh();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    bool FileExplorerPanel::IsSelected( const DirectoryInformation* entry ) const
    {
        return entry && m_Selection.count( entry->AssetPath ) != 0;
    }

    std::vector<std::string> FileExplorerPanel::SelectionPaths() const
    {
        if ( !m_Selection.empty() )
            return std::vector<std::string>( m_Selection.begin(), m_Selection.end() );
        if ( m_CurrentSelected )
            return { m_CurrentSelected->AssetPath };
        return {};
    }

    void FileExplorerPanel::SelectClick( DirectoryInformation* entry, int shownIndex )
    {
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyShift && m_SelectionAnchorShown >= 0 && m_CurrentDir )
        {
            const auto order = BuildDisplayOrder();
            const int  lo    = m_SelectionAnchorShown < shownIndex ? m_SelectionAnchorShown : shownIndex;
            const int  hi    = m_SelectionAnchorShown < shownIndex ? shownIndex : m_SelectionAnchorShown;
            m_Selection.clear();
            for ( int i = lo; i <= hi && i < static_cast<int>( order.size() ); ++i )
                m_Selection.insert( m_CurrentDir->Children[order[i]]->AssetPath );
        }
        else if ( io.KeyCtrl || io.KeySuper ) // Cmd on macOS
        {
            if ( m_Selection.count( entry->AssetPath ) )
                m_Selection.erase( entry->AssetPath );
            else
                m_Selection.insert( entry->AssetPath );
            m_SelectionAnchorShown = shownIndex;
        }
        else
        {
            m_Selection.clear();
            m_Selection.insert( entry->AssetPath );
            m_SelectionAnchorShown = shownIndex;
        }
        m_CurrentSelected = entry;
    }

    void FileExplorerPanel::PasteClipboard()
    {
        if ( m_Clipboard.empty() || !m_CurrentDir )
            return;
        for ( const auto& src : m_Clipboard )
        {
            std::string np, err;
            const bool  ok = m_ClipboardCut
                                 ? AssetFileOps::Move( src, m_CurrentDir->AssetPath, np, err )
                                 : AssetFileOps::CopyInto( src, m_CurrentDir->AssetPath, np, err );
            if ( !ok )
                m_FileOpStatus = "Paste failed: " + err;
        }
        if ( m_ClipboardCut )
            m_Clipboard.clear(); // a cut is consumed by the paste
        QueueRefresh();
    }

    void FileExplorerPanel::CaptureThumbnailFromViewport( const std::string& assetPath )
    {
        const auto scene = m_ViewportScene.lock();
        if ( !scene )
            return;
        const auto img = scene->GetFinalImage(); // the main viewport's post-processed render
        if ( !img )
            return;

        Graphic::Renderer::GetInstance().WaitDeviceIdle(); // readback after the GPU finished the frame
        // THE REFUSAL IS NOW DISTINGUISHABLE FROM AN EMPTY PICTURE. The size check below was the only
        // thing standing between "the readback failed" and "the thumbnail is blank", and it answered
        // both with a silent `return` — so a folder icon that never appeared looked exactly like one the
        // user had not asked for.
        const auto read = img->ReadPixelsRGBA8();
        if ( !read.IsSuccess() )
        {
            LOG_ERROR( "[ContentBrowser] the folder thumbnail was not captured: {}", read.GetError() );
            return;
        }

        const std::vector<uint8_t>& src = read.GetValue();
        const uint32_t              W   = img->GetWidth();
        const uint32_t              H   = img->GetHeight();
        if ( W == 0 || H == 0 || src.size() != static_cast<size_t>( W ) * H * 4 )
        {
            LOG_ERROR( "[ContentBrowser] the folder thumbnail was not captured: the readback returned {} "
                       "byte(s) for a {}x{} image",
                       src.size(), W, H );
            return;
        }

        // Center-crop to a square, then downscale (nearest) to a square thumbnail — frame the asset in the
        // viewport so the centered square captures it.
        const uint32_t       side = ( W < H ) ? W : H; // (avoid std::min — windows.h min macro in this TU)
        const uint32_t       ox   = ( W - side ) / 2;
        const uint32_t       oy   = ( H - side ) / 2;
        constexpr uint32_t   kOut = 512; // NOTE: not 'OUT' — that's a windows.h SAL macro in this TU
        std::vector<uint8_t> out( static_cast<size_t>( kOut ) * kOut * 4 );
        for ( uint32_t y = 0; y < kOut; ++y )
            for ( uint32_t x = 0; x < kOut; ++x )
            {
                const uint32_t sx = ox + x * side / kOut;
                const uint32_t sy = oy + y * side / kOut;
                for ( uint32_t c = 0; c < 4; ++c )
                    out[( ( y * kOut + x ) * 4 ) + c] = src[( ( sy * W + sx ) * 4 ) + c];
            }

        const std::string png = ThumbnailKey::DiskPath( assetPath ); // same key the grid reads
        std::error_code   ec;
        std::filesystem::create_directories( std::filesystem::path( png ).parent_path(), ec );
        stbi_flip_vertically_on_write( 0 ); // viewport readback is already upright (same as the offscreen path)
        stbi_write_png( png.c_str(), kOut, kOut, 4, out.data(), kOut * 4 );
        if ( m_Thumbnails )
            m_Thumbnails->Invalidate( png ); // drop the cached decode so the grid reloads the new image
        LOG_INFO( "[Thumbnail] Captured from viewport -> {}", png );
    }

    bool FileExplorerPanel::RenderFile( int dirIndex, bool folder, int shownIndex, bool gridView )
    {
        DirectoryInformation* entry         = m_CurrentDir->Children[dirIndex];
        bool                  doubleClicked = false;

        const std::string fileName = std::filesystem::path( entry->AssetPath ).filename().string();
        const char*       icon     = folder ? ICON_MDI_FOLDER : IconForType( entry->Type );

        ImGui::PushID( dirIndex );

        if ( gridView )
        {
            const float thumb  = m_GridSize * 0.66f;
            const float cellW  = m_GridSize;
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
            const bool drewThumb =
                 entry->IsFile &&
                 ( ( ( entry->Type == FileType::Texture || entry->Type == FileType::Cubemap ) &&
                     DrawTextureThumbnail( entry, ImVec2( thumb, thumb ) ) ) ||
                   ( entry->Type == FileType::Material &&
                     DrawRenderedMaterialThumbnail( entry, ImVec2( thumb, thumb ) ) ) ||
                   ( entry->Type == FileType::Model &&
                     DrawRenderedMeshThumbnail( entry, ImVec2( thumb, thumb ) ) ) ||
                   ( ( entry->Type == FileType::Cloud || entry->Type == FileType::UITheme ) &&
                     DrawPaintedThumbnail( entry, ImVec2( thumb, thumb ) ) ) );
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

            if ( ImGui::IsItemClicked() )
                SelectClick( entry, shownIndex );
            if ( ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                doubleClicked = true;

            EmitAssetDragSource( *entry );
            DrawItemContextMenu( *entry );

            if ( ImGui::IsItemHovered() && !ImGui::IsDragDropActive() )
                ImGui::SetTooltip( "%s", fileName.c_str() );

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
                    while ( shown.size() > 1 &&
                            ImGui::CalcTextSize( ( shown + "..." ).c_str() ).x > cellW )
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
            const bool   sel   = IsSelected( entry );
            const bool   hover = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect( cmin, cmax );
            if ( hover )
                m_TileHovered = true; // consumed by the empty-click deselect in the body loop
            const ImU32  bg    = sel     ? IM_COL32( 52, 92, 160, 150 )
                                 : hover ? IM_COL32( 255, 255, 255, 24 )
                                         : IM_COL32( 255, 255, 255, 10 );
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
            if ( ImGui::Selectable( label.c_str(), IsSelected( entry ), ImGuiSelectableFlags_AllowDoubleClick ) )
            {
                SelectClick( entry, shownIndex );
                if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                    doubleClicked = true;
            }
            if ( ImGui::IsItemHovered() )
                m_TileHovered = true;
            EmitAssetDragSource( *entry );
            DrawItemContextMenu( *entry );
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
        return doubleClicked;
    }

    void FileExplorerPanel::RefreshCurrentDirectory()
    {
        if ( m_Thumbnails )
            m_Thumbnails->Clear();
        if ( !m_CurrentDir )
            return;

        // The erase below frees the child DirectoryInformation entries, so any raw pointer into them (the
        // selection) would dangle. Remember it by its stable path and re-resolve after the rescan.
        const std::string selectedPath = m_CurrentSelected ? m_CurrentSelected->AssetPath : std::string();
        m_CurrentSelected              = nullptr;

        // Drop cached child entries so they re-process from disk (picks up added/removed files), then
        // re-scan the current directory in place — navigation (m_CurrentDir / the tree) is preserved.
        for ( auto* child : m_CurrentDir->Children )
            if ( child )
                m_Directories.erase( child->AssetPath );
        m_CurrentDir->Children.clear();
        m_CurrentDir->Opened = false;

        ProcessDirectory( m_CurrentDir->AssetPath, m_CurrentDir->Parent, true );
        m_UpdateNavigationPath = true;

        if ( !selectedPath.empty() )
            if ( auto it = m_Directories.find( selectedPath ); it != m_Directories.end() )
                m_CurrentSelected = it->second.get();
    }

    void FileExplorerPanel::Refresh()
    {
        if ( m_Thumbnails )
            m_Thumbnails->Clear();

        std::string currentPath = m_CurrentDir->AssetPath;

        // clear() frees every DirectoryInformation -> drop the raw selection pointer so it can't dangle.
        m_CurrentSelected = nullptr;

        m_Directories.clear();

        // Re-scan the panel's actual root (set in the ctor, e.g. "Resources/"). Previously this reset to a
        // hardcoded "Assets" placeholder, wiping the tree on every refresh.
        std::string baseDirectoryHandle = ProcessDirectory( m_BasePath, nullptr, true );
        m_BaseProjectDir                = m_Directories[baseDirectoryHandle].get();
        ChangeDirectory( m_BaseProjectDir );

        m_UpdateNavigationPath = true;

        m_BaseProjectDir    = m_Directories[baseDirectoryHandle].get();
        m_PreviousDirectory = nullptr;
        m_CurrentDir        = nullptr;

        bool dirFound = false;
        for ( auto& dir : m_Directories )
        {
            if ( dir.first == currentPath )
            {
                m_CurrentDir = dir.second.get();
                dirFound     = true;
                break;
            }
        }
        if ( !dirFound )
            ChangeDirectory( m_BaseProjectDir );
        else
            ChangeDirectory( m_CurrentDir );
    }

    void FileExplorerPanel::DrawPreviewPane()
    {
        DirectoryInformation* entry = m_CurrentSelected;
        if ( !entry || !entry->IsFile )
            return;

        ImGui::Separator();
        ImGui::BeginChild( "##assetPreview", ImVec2( 0.0f, 0.0f ), false );

        const std::filesystem::path path( entry->AssetPath );
        const std::string           name = path.filename().string();

        // Left: visual — a rendered thumbnail when one exists for the type, else the big type icon.
        const ImVec2 thumbSize( 140.0f, 140.0f );
        ImGui::BeginGroup();
        bool drewThumb = false;
        if ( entry->Type == FileType::Texture || entry->Type == FileType::Cubemap )
            drewThumb = DrawTextureThumbnail( entry, thumbSize );
        else if ( entry->Type == FileType::Material )
            drewThumb = DrawRenderedMaterialThumbnail( entry, thumbSize );
        else if ( entry->Type == FileType::Model )
            drewThumb = DrawRenderedMeshThumbnail( entry, thumbSize );
        else if ( entry->Type == FileType::Cloud || entry->Type == FileType::UITheme )
            drewThumb = DrawPaintedThumbnail( entry, thumbSize );
        if ( !drewThumb )
        {
            ImGui::PushStyleColor( ImGuiCol_ChildBg, ImVec4( 0.12f, 0.12f, 0.14f, 1.0f ) );
            ImGui::BeginChild( "##previewIcon", thumbSize, true,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
            const char*  icon = IconForType( entry->Type );
            const ImVec2 sz   = ImGui::CalcTextSize( icon );
            ImGui::SetCursorPos(
                 ImVec2( ( thumbSize.x - sz.x ) * 0.5f, ( thumbSize.y - sz.y ) * 0.5f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, entry->FileTypeColour );
            ImGui::TextUnformatted( icon );
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        // Right: name + type/size line + a text excerpt for text-like assets.
        ImGui::BeginGroup();
        ImGui::TextUnformatted( name.c_str() );
        {
            const auto  typeIt   = s_FileTypesToString.find( entry->Type );
            const char* typeName = typeIt != s_FileTypesToString.end() ? typeIt->second.c_str() : "File";
            if ( entry->FileSize >= std::size_t{ 1024 } * 1024 )
            {
                ImGui::TextDisabled( "%s  |  %.1f MB", typeName, entry->FileSize / ( 1024.0f * 1024.0f ) );
            }
            else
                ImGui::TextDisabled( "%s  |  %.1f KB", typeName, entry->FileSize / 1024.0f );
        }

        const bool textual = entry->Type == FileType::Script || entry->Type == FileType::Material ||
                             entry->Type == FileType::Prefab || entry->Type == FileType::Scene ||
                             entry->Type == FileType::Shader || entry->Type == FileType::Ini;
        if ( textual )
        {
            if ( m_PreviewTextPath != entry->AssetPath )
            {
                // Loaded once per selection change; excerpt only (previewing must never hitch the UI).
                m_PreviewTextPath = entry->AssetPath;
                // An unreadable file previews as its error line rather than as silent emptiness.
                auto preview                 = Common::Utils::FileSystem::ReadFileContent( entry->AssetPath );
                m_PreviewText                = preview ? preview.ExtractValue() : preview.GetError();
                constexpr size_t kMaxPreview = 2048;
                if ( m_PreviewText.size() > kMaxPreview )
                {
                    m_PreviewText.resize( kMaxPreview );
                    m_PreviewText += "\n...";
                }
            }

            if ( entry->Type == FileType::Prefab )
            {
                // Cheap structural hint: every serialized entity carries one "Tag" key.
                size_t entities = 0;
                for ( size_t pos = 0; ( pos = m_PreviewText.find( "\"Tag\"", pos ) ) != std::string::npos;
                      ++entities, ++pos )
                    ;
                if ( entities > 0 )
                    ImGui::TextDisabled( "~%zu entities", entities );
            }

            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.65f, 0.65f, 0.65f, 1.0f ) );
            ImGui::BeginChild( "##previewText", ImVec2( 0.0f, 0.0f ), false,
                               ImGuiWindowFlags_HorizontalScrollbar );
            ImGui::TextUnformatted( m_PreviewText.c_str() );
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();

        ImGui::EndChild();
    }

} // namespace Desert::Editor
#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/FileExplorer/AssetContextMenu.hpp>
#include <Editor/Panels/FileExplorer/AssetViewState.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserAssetView.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserCommands.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserHistory.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserPathView.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserSelection.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserToolbar.hpp>
#include <Editor/Panels/FileExplorer/ContentDirectoryModel.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Panels/FileExplorer/FileType.hpp>
#include <Editor/Panels/FileExplorer/NewAssetMenu.hpp>
#include <Editor/Panels/FileExplorer/ThumbnailEditMode.hpp>
#include <Editor/Widgets/ThumbnailPrefetch.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>
#include <Editor/Widgets/ThumbnailWarmup.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/ThumbnailInfo.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <ImGui/imgui.h>
#include <unordered_map>
#include <atomic>
#include <stack>
#include <functional>
#include <future>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    namespace UI
    {
        class UIHelper;
    }
    class ThumbnailCache;
    class AssetThumbnailRenderer;
}

namespace Desert::Core
{
    class Scene; // for "Capture Thumbnail from viewport" (reads the main scene's rendered final image)
}

namespace Desert::Editor
{

    class FileExplorerPanel : public IPanel
    {
    public:
        // @p subjectEditors is what a double-click asks "does anything open this file?". Required, and
        // not defaulted to null: without it every double-click on a document would silently do nothing,
        // which is the exact symptom this panel's own comments say is impossible to diagnose.
        explicit FileExplorerPanel( const std::filesystem::path&         rootPath,
                                    const SubjectEditorRegistry*         subjectEditors,
                                    Assets::AssetManager*                assetManager  = nullptr,
                                    std::weak_ptr<::Desert::Core::Scene> viewportScene = {} );
        ~FileExplorerPanel() override;
        void OnUIRender() override;
        void OnPreUpdate() override; // polls the current dir for external changes -> auto-refresh
        bool OnWindowFileDropped( Common::EventWindowFileDrop& drop );

        /// THE SPLASH'S UPLOAD PASS (THUMB2, THM1n-13). Every picture this panel asked a worker for — the
        /// opening folder's and the whole project's (WarmProjectThumbnails) — that a worker has finished goes
        /// through ThumbnailCache::Get now, the same call the tile makes, so after the hand-over every tile of
        /// every folder finds its picture resident. Nothing is captured or rendered. Returns how many of those
        /// pictures are still waiting for or on a worker.
        std::size_t UploadPrefetchedThumbnails();

        /// WHAT THE SPLASH MAKES RESIDENT (THUMB3, THM1m, THM1n-13). @p scene = ThumbnailWarmup::SceneWarmList,
        /// the open scene's materials and meshes; @p project = ThumbnailWarmup::ProjectWarmList, every picture
        /// of the project from the content registry. Every picture on disk is handed to the prefetch workers
        /// (and uploaded by UploadPrefetchedThumbnails); every one missing or stale is resolved (a mesh on a
        /// worker) and queued with ThumbnailService::WarmMaterial / WarmMesh / WarmPose / WarmPainted, scene
        /// first, for the splash's warm-only capture pass (ThumbnailWarmup::SplashWarmList). Returns how many
        /// captures it queued or is still resolving.
        std::size_t WarmProjectThumbnails( const std::vector<ThumbnailWarmup::WarmItem>& scene,
                                           const std::vector<ThumbnailWarmup::WarmItem>& project );

        /// The splash's captures have landed: hand the project's pictures that are not resident yet — the PNGs
        /// those captures just wrote — to the workers again, so they are uploaded before the hand-over too.
        void RequestProjectPictures();

        /// How many pictures the browser holds on the GPU (ThumbnailCache::ResidentCount).
        [[nodiscard]] std::size_t ResidentThumbnails() const;

        /// Meshes WarmProjectThumbnails found cold (read in flight on a worker): asked again each frame until
        /// each is resident and queued, or refused. Returns how many are still being read — they hold the
        /// hand-over like a queued capture does, within the same budget.
        std::size_t TickWarmMeshes();

        // One entry of the open folder as a tile (@p gridView) or a list row — the asset view's OnDrawTile.
        ContentBrowserAssetView::TileResult RenderFile( int dirIndex, bool folder, int shownIndex, bool gridView );
        // Drag source with a thumbnail/big-icon preview (needs the thumbnail cache, hence a member).
        void EmitAssetDragSource( const DirectoryInformation& entry );

        // Paths of the current multi-selection; falls back to the current entry when empty.
        std::vector<std::string> SelectionPaths() const;

        // Phase-3 navigation.
        void        GoBack();
        void        GoForward();
        // Resolves a visited path to its dir; false when `path` is not a folder under the project.
        bool NavigateToPath( const std::string& path );

        // THE PINNED FOLDERS ARE NOT THIS PANEL'S, and К5 is why they used to look like they were. This
        // class owned a `m_Favorites` vector, a `FavoritesFile()` that spelled out `$HOME/.desertengine`
        // by hand, and a `SaveFavorites()` that truncated that file and reported nothing about the write.
        // They are EditorPreferences::{Current,Is,Toggle}FavouriteFolder now — per project, relative to
        // the assets root, written by the one checked writer editor.json has.

        // UE's "Add Level Sequence": an empty `.dseq` (LevelSequenceAsset::Save) in the open folder, selected
        // once the folder is re-listed. The ONE creation route: the Assets window's context menu and the
        // palette's "Assets / New Level Sequence" (Editor/Core/ContentCreateCommands.hpp) both call this.
        Common::BoolResultStr CreateNewLevelSequence();

        void ChangeDirectory( DirectoryInformation* directory );
        // Hand the pictures of m_CurrentDir's tiles to ThumbnailPrefetch so a worker decodes them before
        // they are drawn — at startup that is during the splash. Mirrors the per-type branches of the grid.
        void PrefetchCurrentFolderThumbnails();
        // Re-scan ONLY the current directory in place (keeps navigation; used by the watcher + QueueRefresh).
        void RefreshCurrentDirectory();
        void QueueRefresh()
        {
            m_Refresh = true;
        }
        // Opens the rename dialog on the selected asset (the palette's "Assets / Rename the selected
        // asset"); refused, by name, when nothing is selected.
        Common::BoolResultStr RenameSelected();

        // The entries the window shows in its current folder, and the two clicks on one: select it (what F2
        // then renames) and, for a folder, open it. The palette offers these per shown entry, so a client on
        // the control channel reaches an asset the way a click does.
        std::vector<std::string> ShownEntries( bool folders ) const;
        // The selected entries whose thumbnail has an editable orbit (ThumbnailProducers::HasThumbnailOrbit): the
        // palette's "Edit Thumbnail: <file> <step>" commands are offered for these. `Asset` is the entry as the
        // browser lists it, `OrbitFile` the file its picture's orbit is read from and written to
        // (ThumbnailEditMode::OrbitFileOf).
        using ThumbnailOrbitSubject = ThumbnailEditMode::Subject;
        std::vector<ThumbnailOrbitSubject> SelectedThumbnailSubjects();
        Common::BoolResultStr              SelectEntry( const std::string& path );
        // UE's Content Browser navigation (SyncBrowserToFolders / SyncBrowserToAssets), for the palette and the
        // control channel: "Content Browser / Go to Folder <path>" opens a folder anywhere under the browser's
        // root, "Content Browser / Sync to Asset <path>" opens the asset's folder and selects it. Refused, by
        // path, for a path that is not a folder / not a file under the root.
        Common::BoolResultStr GoToFolder( const std::string& path );
        Common::BoolResultStr SyncToAsset( const std::string& path );
        // Every folder / every file under the browser's root, as the browser spells them (generic paths, its
        // hidden-file rule applied): the palette offers one Go to Folder / Sync to Asset per entry.
        std::vector<std::string> ContentFolders() const;
        std::vector<std::string> ContentFiles() const;
        // The Content Browser commands (ContentBrowserCommands.hpp) on the selection: the item context menu
        // draws them through CommandMenuItem and the palette offers them; both land here. Refused, with the
        // reason, when the selection does not fit the command.
        Common::BoolResultStr RunCommand( ContentBrowserCommand command );

    private:
        // OnUIRender's pieces that are not views: the selection's keyboard shortcuts, the last file-op error
        // line, the queued re-listing, and the body's background menu (the asset view's OnBackgroundContextMenu).
        void DrawFileOpStatus();
        void ApplyPendingRefresh();
        // The body's background menu: Paste, Import, Refresh, then NewAssetMenu's "New…" items.
        void DrawBackgroundContextMenu();

        // Content-Browser left pane (folder tree + favorites) width; dragged via the splitter, remembered
        // for the session. Clamped to [kMinTreeWidth, avail - kMinContentWidth] each frame.
        float m_TreeWidth = 240.0f;

        // THE THREE VIEWS (F7; UE SPathView / SNavigationBar / SAssetView) and the one state they share.
        AssetViewState          m_ViewState;
        ContentBrowserPathView  m_PathView;
        ContentBrowserToolbar   m_Toolbar;
        ContentBrowserAssetView m_AssetView;

        bool m_Refresh = false;
        // A file this panel just created, selected by the refresh that lists it (the entry does not exist
        // before that re-listing, so it cannot be selected at creation).
        std::string m_SelectAfterRefresh;

        // THE TREE (F3): every node of the browser, owned here; the raw views below point into it.
        ContentDirectoryModel m_Model;
        // The folder on screen; null only before the model's root was listed.
        DirectoryInformation* m_CurrentDir = nullptr;

        std::string m_FileOpStatus; // last error line (shown above the browser), any piece reports into it

        // Phase-3 navigation/UX.
        ContentBrowserHistory m_History;         // visited folder paths (back/forward)

        Assets::AssetManager*           m_AssetManager = nullptr;
        // WHICH FILES ARE DOCUMENTS, and how each becomes a subject. Non-owning; the registry is a member
        // of EditorLayer and outlives every panel. See Editor/Core/SubjectEditorRegistry.hpp — the chain of
        // `else if` per format that used to live in this file is registered there now, beside the editors.
        const SubjectEditorRegistry*             m_SubjectEditors = nullptr;
        std::unique_ptr<UI::UIHelper>   m_UIHelper;
        std::unique_ptr<ThumbnailCache>          m_Thumbnails;
        // What PrefetchCurrentFolderThumbnails last handed to the workers: the one list the splash's upload
        // pass reads, so "which folder opens" and "which pictures it shows" are never asked twice.
        std::vector<ThumbnailPrefetch::Item> m_PrefetchItems;
        std::vector<ThumbnailPrefetch::Item>
             m_ProjectPrefetchItems; // WarmProjectThumbnails' pictures, decoded too
        std::vector<ThumbnailWarmup::WarmItem>
             m_WarmMeshesPending; // TickWarmMeshes: cold meshes/poses still being read

        // PER-TILE WORK THAT USED TO BE REDONE EVERY FRAME FOR EVERY TILE (THUMB3, sampled in a folder of 240
        // materials): the cache file name costs a StableKeyForPath (std::filesystem::absolute) and the
        // request a material resolve, a registry lookup and two more keys. The name never changes for a
        // path; a request, once accepted, is the service's to finish — asked again only after the picture
        // has been seen current, so an edit that makes it stale asks again.
        const std::string&                           ThumbnailPngFor( const std::string& assetPath );
        std::unordered_map<std::string, std::string> m_ThumbnailPngOf;
        std::unordered_map<std::string, ImVec4>      m_CaptureAsked; // asset path -> its placeholder swatch
        // The constructor's own navigations are not the user's and are not remembered.
        bool m_RestoringFolder = true;

        std::weak_ptr<::Desert::Core::Scene>     m_ViewportScene; // for "Capture Thumbnail from viewport"
        std::unordered_set<std::string>          m_FailedThumbs;  // assets that failed to load -> show icon, no retry spam

        // THE FILE A RenderedMesh TILE PHOTOGRAPHS: a model's own path (CookPaths::MeshAsset maps it to its
        // .stmesh), or the cooked mesh a .defoliage names (ThumbnailFoliage — UE: a foliage type's picture is
        // its mesh's), so a type and its mesh share one key, one freshness source and one capture. The foliage
        // read is cached per path and file time; a refusal is logged once and blacklisted in m_FailedThumbs.
        std::optional<std::string> MeshSourceFor( const DirectoryInformation& entry );
        // The same answer by path and type — for a registry row, which has no DirectoryInformation.
        std::optional<std::string> MeshSourceFor( const std::string& assetPath, FileType type );
        struct MeshSourceRead
        {
            std::filesystem::file_time_type Written;
            std::string                     Source;
        };
        std::unordered_map<std::string, MeshSourceRead> m_MeshSourceOf;

        // WHAT A RenderedMesh TILE SHOWS, by the asset its import wrote (THM-FIXB2; UE: a source file is not an
        // asset — the picture is the imported asset's). A raw source (.fbx/.glb/.gltf/…) is pictured by what its
        // import record says it imports as: a StaticMesh by its cooked .stmesh, a SkinnedMesh by its .skmesh in
        // its bind pose, a Skeleton by its .skeleton on its preview mesh; clips only, or a source not imported
        // yet (no record), keep the type icon — never "has not been cooked". Any other mesh file is its own
        // cooked form. `Cooked` is the picture's key and freshness source; `Pose` routes it to RequestPose.
        struct MeshPicture
        {
            std::string Cooked;
            bool        Pose = false;
        };
        std::optional<MeshPicture> MeshPictureFor( const std::string& assetPath, FileType type );
        // The record read, cached per source and the record's file time (a re-import rewrites it).
        struct SourcePictureRead
        {
            std::filesystem::file_time_type Written;
            std::optional<MeshPicture>      Picture;
        };
        std::unordered_map<std::string, SourcePictureRead> m_SourcePictureOf;

        // File watcher: cheap throttled poll of the current dir's entry signature -> QueueRefresh on change.
        DirectoryWatcher m_Watcher;

        // Resolve (existing-only) + draw a texture thumbnail for an entry; returns false if none.
        // The tile's and the tooltip's picture, by ThumbnailProducers::ProducerOf — the one dispatch. False =
        // draw the type icon (no picture yet, or none by design).
        bool DrawThumbnailFor( DirectoryInformation* entry, const ImVec2& size );
        bool DrawTextureThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // Draw a rendered preview for a material entry (material-on-sphere). Generates the PNG lazily
        // (throttled to ~1/frame) on first use and caches it to disk; returns false until the PNG exists.
        bool DrawRenderedMaterialThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // Same, for a mesh entry (the mesh auto-framed by its bounds).
        bool DrawRenderedMeshThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // Same, for a skinned mesh in its bind pose (ThumbnailPose; the .skmesh is its own cooked form).
        // @p subject is the posed asset: the entry itself, or the .skmesh/.skeleton a skinned source's import
        // wrote.
        bool DrawRenderedPoseThumbnail( DirectoryInformation* entry, const ImVec2& size,
                                        const std::string& subject );
        // Same, for a file whose picture is PAINTED from its own bytes rather than rendered — the four
        // cloud formats. It asks for no handle and no renderer; see Editor/Widgets/CloudThumbnail.hpp.
        bool DrawPaintedThumbnail( DirectoryInformation* entry, const ImVec2& size );

        // UE-style hover tooltip for a tile: picture, name, type/size, path. Shown after the cursor has
        // rested AssetTooltipLayout::kHoverDelaySeconds on the same tile; size and placement from
        // AssetTooltipLayout::Compute (capped, never off-window). A click only selects.
        void DrawAssetTooltip( DirectoryInformation* entry );

        // Which tile the cursor rests on and since when; identity only, never dereferenced here.
        const DirectoryInformation* m_TooltipEntry      = nullptr;
        double                      m_TooltipHoverStart = 0.0;

        // THE PIECES (F6 F8 F9 F10; UE NewAssetOrClassContextMenu / AssetContextMenu / SThumbnailEditModeTools):
        // the panel owns them and calls them. Last, because their constructors read m_AssetManager and
        // m_ViewportScene above.
        ContentBrowserSelection m_Selection;
        ThumbnailEditMode       m_ThumbnailEdit;
        NewAssetMenu            m_NewAssetMenu;
        AssetContextMenu        m_ItemMenu;
    };

} // namespace Desert::Editor
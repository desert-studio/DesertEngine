#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/FileExplorer/AssetContextMenu.hpp>
#include <Editor/Panels/FileExplorer/AssetThumbnailPool.hpp>
#include <Editor/Panels/FileExplorer/AssetTileThumbnail.hpp>
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
        // @p thumbnailPool is the editor's (EditorLayer owns it — UE: the Content Browser uses the editor's
        // FAssetThumbnailPool); the panel draws from it and must not outlive it.
        explicit FileExplorerPanel( const std::filesystem::path& rootPath,
                                    const SubjectEditorRegistry* subjectEditors, AssetThumbnailPool& thumbnailPool,
                                    Assets::AssetManager*                assetManager  = nullptr,
                                    std::weak_ptr<::Desert::Core::Scene> viewportScene = {} );
        ~FileExplorerPanel() override;
        void OnUIRender() override;
        void OnPreUpdate() override; // polls the current dir for external changes -> auto-refresh
        bool OnWindowFileDropped( Common::EventWindowFileDrop& drop );

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
        // THE PICTURES (F5; UE FAssetThumbnailPool + SAssetThumbnail): the pool holds the resident pictures,
        // the workers' lists and the per-path answers, and is the EDITOR's — EditorLayer owns it and the
        // splash (EditorStartup) drives it directly; the drawer draws a tile, its tooltip and a drag's
        // preview from it.
        AssetThumbnailPool& m_ThumbnailPool;
        AssetTileThumbnail m_TileThumbnail;

        // The constructor's own navigations are not the user's and are not remembered.
        bool m_RestoringFolder = true;

        std::weak_ptr<::Desert::Core::Scene> m_ViewportScene; // for "Capture Thumbnail from viewport"

        // File watcher: cheap throttled poll of the current dir's entry signature -> QueueRefresh on change.
        DirectoryWatcher m_Watcher;

        // THE PIECES (F6 F8 F9 F10; UE NewAssetOrClassContextMenu / AssetContextMenu / SThumbnailEditModeTools):
        // the panel owns them and calls them. Last, because their constructors read m_AssetManager and
        // m_ViewportScene above.
        ContentBrowserSelection m_Selection;
        ThumbnailEditMode       m_ThumbnailEdit;
        NewAssetMenu            m_NewAssetMenu;
        AssetContextMenu        m_ItemMenu;
    };

} // namespace Desert::Editor
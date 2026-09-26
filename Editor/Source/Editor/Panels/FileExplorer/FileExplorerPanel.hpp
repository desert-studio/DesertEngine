#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <ImGui/imgui.h>
#include <atomic>
#include <stack>
#include <functional>
#include <future>
#include <memory>
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
    class ThumbnailSweeper;
}

namespace Desert::Core
{
    class Scene; // for "Capture Thumbnail from viewport" (reads the main scene's rendered final image)
}

namespace Desert::Editor
{

    enum class FileType
    {
        Unknown = 0,
        Scene,
        Prefab,
        Script,
        Audio,
        Shader,
        Texture,
        Cubemap,
        Model,
        Material,
        ShaderGraph,
        // `Project` USED TO SIT HERE AND WAS DEAD IN BOTH DIRECTIONS: no extension mapped to it and no
        // code read it. It could not have worked either — a `.deproj` lives at the PROJECT root, above
        // the assets root this browser is rooted at, so the tile it typed can never be drawn.
        Ini,
        Font,

        /// The four cloud formats — `.dclayout`, `.dcnv`, `.dcmv`, `.decloudtype`.
        ///
        /// ONE TYPE FOR FOUR EXTENSIONS, and the alternative was four. They share a colour, an icon, a
        /// filter entry and — the reason that decides it — a THUMBNAIL PRODUCER: all four are painted
        /// from their own bytes by Editor/Widgets/CloudThumbnail.hpp, so every branch that would
        /// distinguish them here would immediately re-join. What tells them apart is the document each
        /// one opens, and that is the subject-editor registry's question, not this enum's.
        ///
        /// THEY WERE `Unknown` UNTIL M11, which is why the owner could not pick a cloud by looking: an
        /// unknown type gets the generic document glyph, so four different assets drew one identical
        /// grey square and the browser's own type filter could not name them.
        Cloud,

        /// A UI theme (`.detheme`) — named colours, metrics and fonts plus the styles that bind them.
        /// Its OWN type rather than sharing one: it has no producer in common with anything above (a
        /// theme is not painted from bytes the way a cloud is), and the browser's type filter has to be
        /// able to name it, which is the whole reason the cloud formats stopped being `Unknown`.
        UITheme
    };

    struct DirectoryInformation
    {
        DirectoryInformation*              Parent;
        std::vector<DirectoryInformation*> Children;

        std::string AssetPath;
        // SharedPtr<Graphics::Texture2D> Thumbnail = nullptr;
        FileType Type;
        uint64_t FileSize;
        uint64_t LastWriteTime = 0; // filesystem mtime (for "sort by date"); cached so sorting needs no syscalls
        ImVec4   FileTypeColour;

        bool Hidden = false;
        bool IsFile = true;
        bool Opened = false;
        bool Leaf   = true;

        // Lazily-resolved texture thumbnail handle (0 = none / not a registered texture). Cached so the
        // grid doesn't re-resolve every frame; resolution is existing-only (browsing never cooks).
        uint64_t ThumbnailHandle   = 0;
        bool     ThumbnailResolved = false;

    public:
        DirectoryInformation( const std::string& path, bool isFile )
        {
            AssetPath = path;
            IsFile    = isFile;
            Hidden    = false;
        }

        ~DirectoryInformation()
        {
        }
    };
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
        void OnEvent( Common::Event& e ) override; // OS file drop -> import into the current dir

        bool RenderFile( int dirIndex, bool folder, int shownIndex, bool gridView );
        // Right-click context menu on a file/folder: Open (default app), Show in Explorer, Open folder, etc.
        void DrawItemContextMenu( DirectoryInformation& entry );
        // Modal dialogs for the cross-platform file ops (rename / delete-with-reference-warning).
        void DrawFileOpsPopups();
        // Multi-select click handling (plain / Ctrl-toggle / Shift-range over the display order).
        void SelectClick( DirectoryInformation* entry, int shownIndex );
        bool IsSelected( const DirectoryInformation* entry ) const;
        // Drag source with a thumbnail/big-icon preview (needs the thumbnail cache, hence a member).
        void EmitAssetDragSource( const DirectoryInformation& entry );

        // True while any tile (card rect / list row) is hovered this frame — clicking elsewhere in the
        // body clears the selection (the ScrollY table is a child window, so an item-based check can't work).
        bool m_TileHovered = false;
        // Paths of the current multi-selection; falls back to m_CurrentSelected when empty.
        std::vector<std::string> SelectionPaths() const;
        // Cut/copy/paste of the current selection into the current directory.
        void PasteClipboard();

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

        // Phase-4 engine integration: instantiate a prefab into the open scene; create a new material asset.
        void AddPrefabToScene( const std::string& prefabPath );
        void CreateNewMaterial();

        /// Which of the four cloud formats a "New Cloud Asset" item creates.
        ///
        /// AN ENUM AND NOT FOUR METHODS, because everything around the creation — the unique name, the
        /// directory, the refusal, the refresh, the document that opens afterwards — is identical for all
        /// four and only the payload differs. Four methods would be four copies of that surround, which is
        /// how three of them come to lack the error path.
        enum class CloudAssetKind
        {
            Type,            ///< `.decloudtype` — numbers only; written in the handler
            Layout,          ///< `.dclayout` — a blank painting; written in the handler
            NoiseVolume,     ///< `.dcnv` — 8 MiB of voxels, GENERATED on a worker (8.7 s in Debug)
            ModellingVolume, ///< `.dcmv` — 4 MiB of voxels, BAKED on a worker (1.6 s in Debug)
        };

        // Creates one cloud asset in the current directory under a unique name and opens its document.
        // The two volume formats are generated on a worker; see m_CloudBake.
        void CreateNewCloudAsset( CloudAssetKind kind );
        // UE-style "Capture Thumbnail": grab the current main-viewport rendered image, center-crop to a
        // square, downscale, and save it AS this asset's thumbnail (same DiskPath key the grid reads). Lets
        // the user frame the asset in the scene and use that exact view as the preview.
        void CaptureThumbnailFromViewport( const std::string& assetPath );
        // Filtered (m_SearchBuf) + sorted (m_SortMode) child indices for the current directory.
        std::vector<size_t> BuildDisplayOrder() const;
        void DrawFolder( DirectoryInformation* dirInfo, bool defaultOpen = false );

        void DestroyGraphicsResources()
        {
            /* m_FolderIcon.reset();
             m_FileIcon.reset();
             m_Directories.clear();*/
        }

        std::string ProcessDirectory( const std::string& directoryPath, DirectoryInformation* parent,
                                      bool processChildren );

        void ChangeDirectory( DirectoryInformation* directory );
        void RemoveDirectory( DirectoryInformation* directory, bool removeFromParent = true );
        // void OnNewProject() override;
        void Refresh();
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
        Common::BoolResultStr    SelectEntry( const std::string& path );
        Common::BoolResultStr    OpenFolder( const std::string& path );

    private:
        // Collects a finished cloud-volume generation, exactly once. Called from OnPreUpdate rather than
        // from the render so that a collapsed or hidden Assets window still finishes what it started.
        void PollCloudAssetBake();

        // Reports the outcome of a creation and, on success, opens the new file's document. One place, so
        // the cheap formats and the generated ones cannot come to report differently.
        void FinishCloudAsset( const Common::BoolResultStr& written );

        // The visible sign that a file is still being made. Without it the two volume formats look like a
        // menu item that did nothing for several seconds.
        void DrawCloudAssetBakeStatus();

    private:
        std::filesystem::path m_CurrentPath;

        float       m_MinGridSize = 50;
        float       m_MaxGridSize = 400;
        std::string m_MovePath;
        std::string m_LastNavPath;
        std::string m_Delimiter;

        size_t m_BasePathLen;
        bool   m_IsDragging;
        bool   m_IsInListView;
        bool   m_UpdateBreadCrumbs;
        bool   m_ShowHiddenFiles;
        int    m_GridItemsPerRow;
        float  m_GridSize = 360.0f;

        // Content-Browser left pane (folder tree + favorites) width; dragged via the splitter, remembered
        // for the session. Clamped to [kMinTreeWidth, avail - kMinContentWidth] each frame.
        float m_TreeWidth = 240.0f;

        // Asset filtering + sorting (toolbar).
        enum class SortMode
        {
            Name = 0,
            DateModified,
            Type,
            Size
        };
        char     m_SearchBuf[128] = { 0 };
        SortMode m_SortMode        = SortMode::Name;
        bool     m_SortDescending  = false;

        ImGuiTextFilter m_Filter;

        bool m_TextureCreated = false;

        std::string m_BasePath;
        std::string m_AssetPath;

        bool m_Refresh = false;

        bool m_UpdateNavigationPath = true;

        // Default-initialized: the ctor omitted m_CurrentSelected / m_NextDirectory, so they held
        // indeterminate values and the first frame could deref garbage (a layout-dependent crash in
        // OnUIRender). Also nulled on Refresh so a rebuilt m_Directories never leaves a dangling selection.
        DirectoryInformation* m_CurrentDir        = nullptr;
        DirectoryInformation* m_BaseProjectDir    = nullptr;
        DirectoryInformation* m_NextDirectory     = nullptr;
        DirectoryInformation* m_PreviousDirectory = nullptr;

        std::unordered_map<std::string, std::shared_ptr<DirectoryInformation>> m_Directories;
        std::vector<DirectoryInformation*>                                     m_BreadCrumbData;
        /* TDArray<DirectoryInformation*>                                                     m_BreadCrumbData;
         SharedPtr<Graphics::Texture2D>                                                     m_FolderIcon;
         SharedPtr<Graphics::Texture2D>                                                     m_FileIcon;*/

        DirectoryInformation* m_CurrentSelected = nullptr;

        std::string m_RequestedThumbnailPath;
        std::string m_CopiedPath;
        bool        m_CutFile = false;

        // Phase-1 file operations (rename / delete / duplicate / move), cross-platform.
        bool                     m_ShowRenamePopup   = false;
        std::string              m_RenamePath;
        char                     m_RenameBuf[128]    = { 0 };
        std::vector<std::string> m_RenameReferrers; // registry keys that keep loading through the redirector
        bool                     m_ShowDeleteConfirm = false;
        std::vector<std::string> m_PendingDeleteList; // paths queued for the delete-confirm modal
        std::vector<std::string> m_DeleteReferencers; // assets still pointing at the delete target(s)
        std::string              m_FileOpStatus;      // last error line (shown in the toolbar area)

        // Phase-2 multi-select + clipboard.
        std::unordered_set<std::string> m_Selection;            // selected asset paths
        int                             m_SelectionAnchorShown = -1; // display index of the range anchor
        std::vector<std::string>        m_Clipboard;            // cut/copied paths
        bool                            m_ClipboardCut = false; // true = move on paste, false = copy

        // Phase-3 navigation/UX.
        int                      m_TypeFilter = -1;      // FileType value to show, or -1 for "All"
        std::vector<std::string> m_NavHistory;           // visited folder paths (back/forward)
        int                      m_NavPos            = -1;
        bool                     m_NavigatingHistory = false; // suppress history push during back/forward

        Assets::AssetManager*           m_AssetManager = nullptr;
        // WHICH FILES ARE DOCUMENTS, and how each becomes a subject. Non-owning; the registry is a member
        // of EditorLayer and outlives every panel. See Editor/Core/SubjectEditorRegistry.hpp — the chain of
        // `else if` per format that used to live in this file is registered there now, beside the editors.
        const SubjectEditorRegistry*             m_SubjectEditors = nullptr;
        std::unique_ptr<UI::UIHelper>   m_UIHelper;
        std::unique_ptr<ThumbnailCache>          m_Thumbnails;

        // ── THE PICTURES MAKE THEMSELVES ──────────────────────────────────────────────────────────────
        //
        // Walks this panel's root on a worker and queues a thumbnail for everything the browser can show
        // and has no usable picture of — see Editor/Widgets/ThumbnailSweep.hpp for the three triggers it
        // answers and for its relation to AssetPreloader's walk.
        //
        // OWNED BY THIS PANEL, and the reason is that the sweep's authority on "what this project
        // contains" is THIS PANEL'S ROOT: it pictures what the Content Browser can show, so the browser
        // is the thing that knows what to sweep. It is driven from OnPreUpdate rather than from the
        // render, so a collapsed, hidden or closed Assets window does not stop the previews arriving —
        // which is the whole point of a background sweep and the same reasoning PollCloudAssetBake gives
        // next door.
        std::unique_ptr<ThumbnailSweeper>        m_Sweeper;
        std::weak_ptr<::Desert::Core::Scene>     m_ViewportScene; // for "Capture Thumbnail from viewport"
        std::unordered_set<std::string>          m_FailedThumbs;  // assets that failed to load -> show icon, no retry spam

        // File watcher: cheap throttled poll of the current dir's entry signature -> QueueRefresh on change.
        int    m_PollCounter   = 0;
        size_t m_DirSignature  = 0;

        bool m_IsHovered = false; // is the Assets window hovered this frame (gates OS file-drop import)

        // Copy an external image into Resources/Textures, then import+register it (Import button).
        void ImportExternalTexture();
        // Copy one external file into the current dir; cook+register if it's a texture (drag-drop / import).
        void ImportExternalFile( const std::filesystem::path& src );
        // Resolve (existing-only) + draw a texture thumbnail for an entry; returns false if none.
        bool DrawTextureThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // Draw a rendered preview for a material entry (material-on-sphere). Generates the PNG lazily
        // (throttled to ~1/frame) on first use and caches it to disk; returns false until the PNG exists.
        bool DrawRenderedMaterialThumbnail( DirectoryInformation* entry, const ImVec2& size );
        // Same, for a mesh entry (the mesh auto-framed by its bounds).
        bool DrawRenderedMeshThumbnail( DirectoryInformation* entry, const ImVec2& size );
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

        // ── Creating a cloud volume: the one generation this panel may have in flight ──────────────────
        //
        // A FUTURE RATHER THAN A RAW THREAD, and one rather than many. The future is what makes the result
        // collected exactly once and the destructor able to guarantee that nothing is still writing into
        // these members; the ONE is what makes that guarantee cheap — a second click would otherwise
        // overwrite the future, detach a running thread, and leave it storing into a progress counter a
        // different bake is already reading. The four menu items are disabled while this is true.
        std::future<Common::BoolResultStr> m_CloudBake;
        bool                               m_CloudBakeRunning = false;

        /// 0..1, written by the worker and read by the frame — hence atomic.
        std::atomic<float> m_CloudBakeProgress{ 0.0f };

        /// Asks a running `.dcmv` bake to stop. It is what makes the destructor bounded rather than a wait
        /// on a whole bake; `GenerateCloudNoiseVolume` has no such hook, so a `.dcnv` in flight is waited
        /// out in full — see the destructor.
        std::atomic<bool> m_CloudBakeCancelled{ false };

        std::string m_CloudBakePath;  ///< where the running creation will write
        std::string m_CloudBakeLabel; ///< its file name, for the status line
    };

} // namespace Desert::Editor
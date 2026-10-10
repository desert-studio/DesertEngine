#pragma once

#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    struct DirectoryInformation;

    /// WHAT THE CONTENT BROWSER CAN MAKE (UE: NewAssetOrClassContextMenu + the "Add to Level" of a prefab): the
    /// "New…" items of the body's background menu — a folder, a material, a level sequence, a shader graph by
    /// domain, the four cloud formats — each written into the open folder under a unique name, and a prefab
    /// placed into the open scene. What a new cloud asset CONTAINS is NewCloudAsset's; this is where it is
    /// written, which file it is, and how it is reported.
    class NewAssetMenu
    {
    public:
        /// Which of the four cloud formats a "New Cloud Asset" item creates.
        ///
        /// AN ENUM AND NOT FOUR METHODS, because everything around the creation — the unique name, the
        /// directory, the refusal, the refresh, the document that opens afterwards — is identical for all
        /// four and only the payload differs.
        enum class CloudAssetKind
        {
            Type,            ///< `.decloudtype` — numbers only; written in the handler
            Layout,          ///< `.dclayout` — a blank painting; written in the handler
            NoiseVolume,     ///< `.dcnv` — 8 MiB of voxels, GENERATED on a worker (8.7 s in Debug)
            ModellingVolume, ///< `.dcmv` — 4 MiB of voxels, BAKED on a worker (1.6 s in Debug)
        };

        struct Delegates
        {
            std::function<void()>                     OnRefresh;            // re-list the open folder
            std::function<void( const std::string& )> OnSelectAfterRefresh; // select this path once listed
            std::function<void( const std::string& )> OnStatus;             // the red line; empty clears it
        };

        NewAssetMenu( Assets::AssetManager* assetManager, std::weak_ptr<::Desert::Core::Scene> viewportScene,
                      Delegates delegates );
        // CANCEL, THEN WAIT: the worker writes into this object's progress and path.
        ~NewAssetMenu();
        NewAssetMenu( const NewAssetMenu& )            = delete;
        NewAssetMenu& operator=( const NewAssetMenu& ) = delete;

        /// The "New…" items, creating in @p folder.
        void Draw( const DirectoryInformation& folder );
        /// The visible sign that a cloud volume is still being made (a line and a progress bar).
        void DrawBakeStatus();
        /// Collects a finished cloud-volume generation, exactly once. Called from the panel's pre-update rather
        /// than from the render, so a collapsed or hidden Assets window still finishes what it started.
        void Poll();

        /// Whether "Add to Scene" has a scene and an asset manager to place a prefab with.
        [[nodiscard]] bool CanAddPrefabToScene() const;
        /// Instantiates @p prefabPath into the open scene — under the selected entity when there is one —
        /// through the same undoable path the viewport drop uses.
        void AddPrefabToScene( const std::string& prefabPath );

        void CreateNewFolder( const DirectoryInformation& folder );
        void CreateNewMaterial( const DirectoryInformation& folder );
        /// UE's "Add Level Sequence": an empty `.dseq` in @p folder, selected once the folder is re-listed. The
        /// ONE creation route: the background menu and the palette's "Assets / New Level Sequence" both land here.
        Common::BoolResultStr CreateNewLevelSequence( const DirectoryInformation* folder );
        /// UE's Add > Input > Input Action / Input Mapping Context: an empty `.deinputaction` (Bool, consumes
        /// input) or `.deinputcontext` (no mappings) in @p folder, selected once listed. The ONE creation route
        /// of the background menu and the palette (ContentCreateCommands.hpp).
        Common::BoolResultStr CreateNewInputAction( const DirectoryInformation* folder );
        Common::BoolResultStr CreateNewInputMappingContext( const DirectoryInformation* folder );
        /// One cloud asset in @p folder under a unique name, its document opened; the two volume formats are
        /// generated on a worker (see m_Bake).
        void CreateNewCloudAsset( const DirectoryInformation& folder, CloudAssetKind kind );

    private:
        // Reports the outcome of a cloud creation and, on success, opens the new file's document. One place, so
        // the cheap formats and the generated ones cannot come to report differently.
        void FinishCloudAsset( const Common::BoolResultStr& written );

        Assets::AssetManager*                m_AssetManager = nullptr;
        std::weak_ptr<::Desert::Core::Scene> m_ViewportScene;
        Delegates                            m_On;

        // ── Creating a cloud volume: the one generation the browser may have in flight ──────────────────
        //
        // A FUTURE RATHER THAN A RAW THREAD, and one rather than many. The future is what makes the result
        // collected exactly once and the destructor able to guarantee that nothing is still writing into
        // these members; the ONE is what makes that guarantee cheap — a second click would otherwise
        // overwrite the future, detach a running thread, and leave it storing into a progress counter a
        // different bake is already reading. The four menu items are disabled while this is true.
        std::future<Common::BoolResultStr> m_Bake;
        bool                               m_BakeRunning = false;
        std::atomic<float>                 m_BakeProgress{ 0.0f }; // 0..1, written by the worker
        // Asks a running `.dcmv` bake to stop; a `.dcnv` has no such hook and is waited out (see the dtor).
        std::atomic<bool> m_BakeCancelled{ false };
        std::string       m_BakePath;  // where the running creation will write
        std::string       m_BakeLabel; // its file name, for the status line
    };
} // namespace Desert::Editor

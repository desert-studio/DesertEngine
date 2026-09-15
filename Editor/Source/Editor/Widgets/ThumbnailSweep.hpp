#pragma once

#include <Editor/Widgets/ThumbnailFormats.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @file
     * @brief THE THUMBNAILS MAKE THEMSELVES.
     *
     * WHAT THIS REPLACES. Every rendered thumbnail in this editor used to be produced by a PANEL DRAWING
     * A TILE: the browser walked past a material, so the material acquired a picture. That is a fine rule
     * for the tile you are looking at and a bad one for everything else — a project opened on a machine
     * with no cache showed grey squares until somebody scrolled through every folder in it, and a file
     * dropped into a directory nobody had open stayed a grey square for ever. The owner's requirement is
     * the opposite of that and is quoted here because it is short: the thumbnails "должны асинхронно
     * сами загружаться при старте сцены... а так же при добавлении в директорию новых ассетов когда
     * эдитор запущен".
     *
     * ── THREE TRIGGERS, AND THEY ARE ONE MECHANISM ────────────────────────────────────────────────────
     *
     * The requirement names three moments. They are implemented as ONE periodic scan, and that is a
     * decision rather than a shortcut:
     *
     *   1. A SCENE IS OPENED and some assets have no picture. The first scan runs on the first frame
     *      after the browser knows its root, which is before any scene has finished loading.
     *   2. A FILE APPEARS in the content directory while the editor is running. The scan repeats, so the
     *      file is seen on the next pass.
     *   3. A FILE CHANGES on disk and its picture is now of something else. The scan asks
     *      `ThumbnailFreshness` — the SAME rule every reader uses — so a source newer than its PNG is a
     *      candidate for exactly the same reason the browser refuses to draw it.
     *
     * A NATIVE FILESYSTEM WATCHER WAS CONSIDERED AND REFUSED, and the argument is not "polling was
     * easier". FSEvents on macOS and ReadDirectoryChangesW on Windows are two implementations of an
     * interface that would answer trigger 2 and NEITHER of the other two: a watcher reports that a path
     * changed, so trigger 3 still needs the two stat calls that the scan is made of, and trigger 1 is a
     * question about state rather than about events and needs a full pass whatever else exists. So the
     * choice was between one mechanism that answers all three and two mechanisms of which the platform
     * pair answers one — on two operating systems, with two sets of coalescing semantics, for a saving
     * measured in the milliseconds this file's own note on the interval quantifies.
     *
     * ── ITS RELATION TO AssetPreloader, WRITTEN DOWN BECAUSE THERE ARE NOW TWO WALKS ──────────────────
     *
     * `Assets::AssetPreloader::PreloadCookedAssetsAndMaterials` also enumerates content directories, and
     * the two walks overlap on disk. They are NOT one question and neither is derived from the other:
     *
     *   THE PRELOADER asks "which assets exist, so a handle in a scene can resolve to one?" It runs ONCE
     *   at startup, it CREATES and REGISTERS assets, its roots are the ENGINE's (the cooked tree plus the
     *   material root), and its result is the AssetManager's registry.
     *
     *   THIS SWEEP asks "which files the Content Browser can SHOW have no picture, or an out-of-date
     *   one?" It runs REPEATEDLY, it creates nothing, its root is the BROWSER's (whatever directory the
     *   Content Browser is rooted at), and its result is a queue of thumbnail requests.
     *
     * The set of asset types the preloader registers and the set of formats this sweep pictures are
     * therefore under no obligation to agree, and they do not: the preloader loads `.tex`, `.anim` and
     * `.skeleton`, none of which the browser lists; this sweep pictures four cloud formats the preloader
     * handles in four other functions entirely. What the two DO share is the enumeration itself —
     * `Common::Utils::FileSystem::ListFilesRecursive`, the one implementation of "what files are under
     * this root", which is also the only one that sees a mounted `.dpak`. `Desert/Tests/Editor/
     * ThumbnailSweep` asserts that neither of them grows a private directory walk.
     *
     * ── WHAT IT COSTS, AND WHY IT CANNOT STARVE THE PERSON ────────────────────────────────────────────
     *
     * THIS CLASS CREATES NO RENDERER. It appends to `ThumbnailService`'s queues and nothing else, so the
     * number of renderer slots a running sweep can occupy is exactly the number that service occupies —
     * ONE, and only while `RendererSlotBudget::MayClaim(Background, ...)` allows it. There are six slots
     * (`EngineContext::kMaxRendererSlots`); a sweep at full tilt therefore leaves five for the surfaces a
     * person opens by hand, and the sixth is the one the service is forbidden to take. The four cloud
     * formats are painted on a JobSystem worker and claim NOTHING, so a project of clouds sweeps with all
     * six free. That arithmetic is asserted rather than described — see the suite.
     */

    /// One file the sweep found without a usable picture.
    struct ThumbnailSweepCandidate
    {
        /// The file as the Content Browser lists it.
        std::string AssetPath;

        /// The file that is PHOTOGRAPHED and whose modification time decides freshness. Equal to
        /// AssetPath for everything except a mesh, where it is the cooked `.stmesh` — see
        /// ThumbnailSubject::Mesh::CookedPath for why that side wins.
        std::string Subject;

        /// Where the picture goes: `ThumbnailKey::DiskPath( Subject )`.
        std::string Png;

        ThumbnailFormats::Producer By = ThumbnailFormats::Producer::None;
    };

    /**
     * @brief Everything under @p root that the browser can show and that has no usable picture.
     *
     * FREE OF THE ASSET MANAGER, THE GPU AND ImGui, which is what makes it safe on a JobSystem worker AND
     * drivable by a test over a temporary directory. It reads directory entries and modification times;
     * it decides nothing about assets.
     *
     * BOUNDED BY @p limit, and the bound is not defensive programming. A scan is dispatched every few
     * seconds for the life of the session, and an unbounded result over a cold cache would hand the main
     * thread a hundred thousand strings to walk through at eight per frame — three hours of a queue that
     * cannot respond to a file appearing at the end of it. Taking the first @p limit and coming back is
     * what keeps the loop's period bounded; the assets past the bound are found on the next pass, which
     * runs as soon as this batch drains.
     */
    [[nodiscard]] std::vector<ThumbnailSweepCandidate> ScanForMissingThumbnails( const std::filesystem::path& root,
                                                                                 std::size_t limit );

    class ThumbnailSweeper
    {
    public:
        /// How many requests one frame may hand to `ThumbnailService`.
        ///
        /// The cost of one is a `ThumbnailKey::Identity` (path algebra) plus, for a material or a mesh,
        /// an AssetManager lookup and possibly a create-and-register. Eight is a number chosen so that a
        /// frame that finds eight COLD meshes — the worst case, because a cold mesh parses its cooked
        /// JSON — is still a frame; the queue behind them drains at one capture per ~2 s regardless, so
        /// a larger number would only move the waiting from this side of the queue to the other.
        static constexpr int kRequestsPerFrame = 8;

        /// Frames between the end of one scan and the start of the next. 180 is ~3 s at 60 fps.
        ///
        /// IT IS THE LATENCY OF "I JUST DROPPED A FILE IN", which is the only thing it decides — a scan
        /// runs on a worker and costs the main thread nothing, so a shorter interval buys responsiveness
        /// and spends worker time on a walk whose answer is almost always "nothing to do". Three seconds
        /// is under the time it takes to switch back to the editor window from wherever the file came
        /// from, and it is 180 times cheaper than asking every frame.
        static constexpr int kFramesBetweenScans = 180;

        /// Files one scan may report. See ScanForMissingThumbnails.
        static constexpr std::size_t kCandidatesPerScan = 512;

        /**
         * @brief Drive the sweep. Main thread, once per frame, from the panel that owns the root.
         *
         * @p root is the directory the Content Browser is showing — the sweep's authority on "what this
         * project contains", deliberately the browser's root and not the engine's content roots, because
         * the question it asks is about what a person can SEE in the browser.
         *
         * A null @p manager is a legal state (the panel accepts one), and it means only that materials
         * and meshes cannot be resolved this frame; the cloud formats need no manager and still go.
         */
        void Tick( Assets::AssetManager* manager, const std::filesystem::path& root );

        /// Stop the current pass and forget what it found. Called when the browser's root changes — a
        /// different project's candidates are answers to a question nobody is asking any more.
        void Reset();

        /**
         * @brief Hand at most @ref kRequestsPerFrame candidates to @p sink and return how many went.
         *
         * PUBLIC BECAUSE IT IS THE BUDGET, and a budget that can only be observed by running the editor
         * is a budget nobody can assert. `Desert/Tests/Editor/ThumbnailSweep` fills the pending list and
         * drives this directly.
         */
        int Drain( const std::function<void( const ThumbnailSweepCandidate& )>& sink );

        /// For the suite: seed the pending list without a filesystem behind it.
        void SetPendingForTest( std::vector<ThumbnailSweepCandidate> pending );

        [[nodiscard]] std::size_t PendingCount() const
        {
            return m_Pending.size();
        }

    private:
        std::future<std::vector<ThumbnailSweepCandidate>> m_Scan;
        std::vector<ThumbnailSweepCandidate>              m_Pending;
        std::size_t                                       m_Next = 0;

        /// Counts DOWN to the next scan. Starts at zero so the first Tick with a root scans immediately:
        /// "the editor just opened a project and nothing has a picture" is the state this whole file
        /// exists for, and making it wait three seconds for its first look would be an odd way to start.
        int m_FramesUntilScan = 0;

        /// The root the pending list was found under. A change means the project changed under us.
        std::filesystem::path m_Root;

        /// How many assets THIS pass offered that no earlier pass had. A sweep that says nothing is
        /// indistinguishable from a sweep that is not running, which is the reading somebody will reach
        /// for the first time a thumbnail is missing — and a sweep that says the same thing every three
        /// seconds is a log nobody reads, which comes to the same end by the other road. Measured: the
        /// first version announced 133 assets once every 1.4 s for the whole session, because a scan run
        /// while the capture queue is full finds exactly the assets already in it.
        int  m_OfferedThisPass = 0;
        bool m_Announced       = false;

        /// Assets this sweep has already handed to the service, by path. Two passes over an asset that
        /// has not been captured YET are one finding, not two; and an asset the service has permanently
        /// refused stays "needs a picture" to the freshness rule for ever, so without this it would be
        /// announced on every pass until the editor closed.
        ///
        /// It does NOT gate the hand-over, only the announcement: an asset EDITED after it was offered
        /// must be re-queued, and the service's own dedup is the thing that decides whether that costs a
        /// capture. Cleared with the rest when the project changes.
        std::unordered_set<std::string> m_Offered;
    };
} // namespace Desert::Editor

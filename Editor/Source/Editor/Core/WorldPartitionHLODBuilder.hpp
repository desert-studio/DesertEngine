#pragma once

// BUILD HLODs, THE SERVICE (UE UWorldPartitionHLODsBuilder: the -BuildHLODs / -DeleteHLODs steps the World
// Partition builder commandlet runs, and the editor's Build > Build HLODs runs the same code). One place builds,
// checks and deletes the HLODs saved beside a world, so the World Partition panel, the command palette and the
// builder commandlet cannot drift apart. Nothing here draws.
//
// WHAT A BUILD READS: the world SAVED at the scene's file (scene + entity files, as WorldCook reads it), never the
// scene in memory - the cook and Play check the HLODs against the same files.
//
// INCREMENTAL. Each saved HLOD row carries the checksum of what it was built from (WorldCells::HLODBuildChecksum:
// the cell's records, the saved content of every mesh, material and texture they name, the HLOD layer settings and
// the builder version). A build rebuilds only the rows whose checksum changed and keeps every other row and its
// file, as UE skips an HLOD actor whose hash is unchanged; ForceAll rebuilds every row.
//
// THREE STAGES, none on the editor's thread for longer than a frame (WP-HLOD-HANG: a 598-cell world held it 6 min
// in the first phase and 10 min in the last). Plan, on a JobSystem worker: the world is read and the builders run
// over the stale rows to gather what they bake. The bakes, over editor frames: their landscape tiles' BaseColor
// (LandscapeHLODBake), their mesh HLODs' merged materials (HLODMaterialBake, WP-HLOD-MATBAKE) and their
// MeshApproximate cells' captured textures (HLODApproximateBake, WP-HLOD-APPROX), in that order. Finish, on a
// worker again: the stale rows are built with each HLOD bound to the material its bake wrote, written cell by
// cell, the index saved and everything read back and checked stale-free. A build with nothing to bake goes from
// Plan straight to Finish. The editor answers Build HLODs at once and reports the run as background work
// (Progress).

#include "HLODApproximateBake.hpp"
#include "HLODMaterialBake.hpp"
#include "LandscapeHLODBake.hpp"

#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    // THE ONE WRITER OF AN IMPOSTOR CARD (Build HLODs and the per-mesh Bake Impostor, MeshImpostorBaking): the
    // baked cell's card mesh stored as `HLODMesh_<hash>.stmesh` in `dir`, answered as the block that draws it
    // (MeshGuid/MeshPath + its one M_I material slot).
    [[nodiscard]] Common::ResultStr<::Desert::Assets::InstancedStaticMeshComponentSer>
    StoreBakedImpostorCard( const HLODApproximateBaked& baked, const std::filesystem::path& dir );

    struct HLODBuildOptions
    {
        // UE's Build HLODs with -ForceBuild: every row is rebuilt, whatever its checksum says.
        bool ForceAll = false;
    };

    // The world's saved HLODs against its saved files.
    struct HLODStatus
    {
        bool                     Built = false; // the world has a saved HLOD index
        std::vector<std::string> Stale;         // the rows missing or stale, by unit name (every row when !Built)
        std::vector<std::string> BuiltUnits;    // the index's rows that name a saved file, by unit name
    };

    class WorldPartitionHLODBuilder
    {
    public:
        WorldPartitionHLODBuilder( std::filesystem::path scenePath, HLODBuildOptions options );
        ~WorldPartitionHLODBuilder();
        WorldPartitionHLODBuilder( const WorldPartitionHLODBuilder& )            = delete;
        WorldPartitionHLODBuilder& operator=( const WorldPartitionHLODBuilder& ) = delete;

        /// Starts Build HLODs of the world saved at the scene path: its first phase goes to a JobSystem worker and
        /// this returns at once. Refused here only for a scene file that is not there; a world the build refuses
        /// (no WorldPartition block, no grid with an HLOD layer, a descriptor index that is not the world's...)
        /// ends the build with Error() from Tick. A previous index that does not read is not refused: every row is
        /// built again, and the log says why.
        [[nodiscard]] Common::BoolResultStr Start( const Assets::AssetManager* assets );

        /// One editor frame of the build: polls the worker phase, or advances a bake by a frame. Never blocks on
        /// the worker. True once it has finished; Error() then says whether it failed.
        [[nodiscard]] bool Tick();

        /// Why the finished build failed; empty for a build that succeeded or is still running.
        [[nodiscard]] const std::optional<std::string>& Error() const
        {
            return m_Error;
        }

        /// What a build that succeeded built and wrote, in words (UE's builder log summary); empty before.
        [[nodiscard]] const std::string& Summary() const
        {
            return m_Summary;
        }

        /// Where a running build is, in words (UE's slow-task progress text): its stage, and on a worker phase how
        /// many cells it has done.
        [[nodiscard]] std::string Progress() const;

    private:
        // THE TWO WORKER PHASES (UE builds HLODs in the builder commandlet's own process, never on the editor's
        // game thread). Plan: read the saved world and the previous index, and run the builders over the rows to
        // build to gather what they bake (m_Plan). Finish: build the stale rows (each landscape HLOD bound to the
        // material m_LandscapeBaked holds for it, each merged-material mesh HLOD to its m_MeshBaked one, each
        // approximated cell's shell to its m_Approximated one), write each cell's file, write the index, read
        // back. Both run with the builder's members to themselves: Tick touches nothing they use until m_Work is
        // ready.
        [[nodiscard]] Common::BoolResultStr Plan();
        [[nodiscard]] Common::BoolResultStr Finish();
        // On the editor's thread once Plan has landed: the bakes it gathered, which render over editor frames.
        void StartBakes();
        // Runs @p phase on a JobSystem Background worker as m_Work.
        void RunOnWorker( Common::BoolResultStr ( WorldPartitionHLODBuilder::*phase )() );
        // A stage's bake was refused: every later stage is dropped and the build ends with @p error.
        bool Refuse( std::string error );

        enum class Stage
        {
            Planning, // Plan on the worker
            Baking,   // the bakes over editor frames
            Writing   // Finish on the worker
        };

        struct Planned; // what Plan hands the bakes (defined beside it)

        std::filesystem::path              m_ScenePath;
        HLODBuildOptions                   m_Options;
        const Assets::AssetManager*        m_Assets = nullptr;
        Stage                              m_Stage  = Stage::Planning;
        std::future<Common::BoolResultStr> m_Work; // the worker phase in flight, if any
        std::unique_ptr<Planned>           m_Plan; // Plan's result, until StartBakes takes it
        // Cells built so far by the worker phase in flight (its sink counts them), for Progress.
        std::atomic<std::size_t> m_CellsDone{ 0 };
        // Set by the destructor (the editor closing, a scene unloading the panel): the worker phase's sink refuses
        // the next cell, so the destructor's wait is one cell long, not the rest of the build.
        std::atomic<bool>                  m_Cancel{ false };
        std::unique_ptr<LandscapeHLODBake> m_Bake; // the landscape BaseColor bake of the stale rows' tiles
        // The merged-material bake of the stale rows' mesh HLODs (WP-HLOD-MATBAKE), run after the landscape bake;
        // what the landscape bake wrote waits in m_LandscapeBaked until it is done.
        std::unique_ptr<HLODMaterialBake> m_MeshBake;
        // The MeshApproximate cells' texture capture (WP-HLOD-APPROX, UE UHLODBuilderMeshApproximate's render
        // capture), run last. m_ApproxShells: each approximated cell's shell as Start built it, by
        // Core::HLODApproximationKey, so Finish binds it to its captured material without wrapping it again.
        std::unique_ptr<HLODApproximateBake>                            m_ApproxBake;
        std::map<std::string, ::Desert::Assets::StaticMeshComponentSer> m_ApproxShells;
        // Each landscape HLOD as Plan built and stored it (its mesh already a `HLODMesh_<hash>.stmesh` beside the
        // cells, the block naming it), by tile id and MinVisibleDistance - the two inputs BuildLandscapeHLODMesh
        // sizes it from - so Finish binds its baked material without building the tile a second time (WP-PERF:
        // the second build was most of Finish).
        std::map<std::pair<std::uint64_t, float>, ::Desert::Assets::StaticMeshComponentSer> m_LandscapeShells;
        // What a finished stage wrote, waiting for the later stages.
        std::vector<LandscapeHLODBaked>   m_LandscapeBaked;
        std::vector<HLODMaterialBaked>    m_MeshBaked;
        std::vector<HLODApproximateBaked> m_Approximated;
        bool                              m_Done = false;
        std::optional<std::string>        m_Error;
        std::string                       m_Summary;
    };

    /// Which of the HLOD rows of the world saved at @p scenePath are missing or stale (UE's World Partition
    /// editor "HLODs need to be rebuilt"). Refused, by name, for a world the build would refuse. THE one reader of a
    /// world's HLOD state (UE: one HLOD state per cell): the panel's HLODs section and its map overlay both ask it.
    [[nodiscard]] Common::ResultStr<HLODStatus> HLODStatusOf( const std::filesystem::path& scenePath );

    /// UE's -DeleteHLODs: removes everything Build HLODs saved beside the world at @p scenePath - the index, every
    /// cell's HLOD file and the landscape tiles' baked textures and materials - and tells the content registry
    /// each file is gone. The number of files removed; a world with no saved HLODs removes none.
    [[nodiscard]] Common::ResultStr<std::size_t> DeleteHLODs( const std::filesystem::path& scenePath );
} // namespace Desert::Editor

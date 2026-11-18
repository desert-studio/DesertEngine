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
// TWO PHASES, as before: Start checks the world and starts what the stale rows bake over the next editor frames -
// their landscape tiles' BaseColor (LandscapeHLODBake), their mesh HLODs' merged materials (HLODMaterialBake,
// WP-HLOD-MATBAKE) and their MeshApproximate cells' captured textures (HLODApproximateBake, WP-HLOD-APPROX), in
// that order; Tick finishes them - the stale rows are built with each HLOD bound to the material its bake wrote,
// written cell by cell, the index saved and everything read back and checked stale-free. A build with nothing to
// bake finishes inside Start.

#include "HLODApproximateBake.hpp"
#include "HLODMaterialBake.hpp"
#include "LandscapeHLODBake.hpp"

#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
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

        /// Starts Build HLODs of the world saved at the scene path. Refused, by name, for a world the build refuses
        /// (no WorldPartition block, no grid with an HLOD layer, a descriptor index that is not the world's...). A
        /// previous index that does not read is not refused: every row is built again, and the log says why.
        [[nodiscard]] Common::BoolResultStr Start( const Assets::AssetManager* assets );

        /// One editor frame of the build. True once it has finished; Error() then says whether it failed.
        [[nodiscard]] bool Tick();

        /// Why the finished build failed; empty for a build that succeeded or is still running.
        [[nodiscard]] const std::optional<std::string>& Error() const
        {
            return m_Error;
        }

    private:
        // The second phase: build the stale rows (each landscape HLOD bound to its @p baked material, each
        // merged-material mesh HLOD to its @p meshBaked one, each approximated cell's shell to its @p approximated
        // one), write, read back.
        [[nodiscard]] Common::BoolResultStr Finish( const std::vector<LandscapeHLODBaked>&   baked,
                                                    const std::vector<HLODMaterialBaked>&    meshBaked,
                                                    const std::vector<HLODApproximateBaked>& approximated );
        // A stage's bake was refused: every later stage is dropped and the build ends with @p error.
        bool Refuse( std::string error );

        std::filesystem::path              m_ScenePath;
        HLODBuildOptions                   m_Options;
        std::unique_ptr<LandscapeHLODBake> m_Bake; // the landscape BaseColor bake of the stale rows' tiles
        // The merged-material bake of the stale rows' mesh HLODs (WP-HLOD-MATBAKE), run after the landscape bake;
        // what the landscape bake wrote waits in m_LandscapeBaked until it is done.
        std::unique_ptr<HLODMaterialBake> m_MeshBake;
        // The MeshApproximate cells' texture capture (WP-HLOD-APPROX, UE UHLODBuilderMeshApproximate's render
        // capture), run last. m_ApproxShells: each approximated cell's shell as Start built it, by
        // Core::HLODApproximationKey, so Finish binds it to its captured material without wrapping it again.
        std::unique_ptr<HLODApproximateBake>                            m_ApproxBake;
        std::map<std::string, ::Desert::Assets::StaticMeshComponentSer> m_ApproxShells;
        // What a finished stage wrote, waiting for the later stages.
        std::vector<LandscapeHLODBaked>   m_LandscapeBaked;
        std::vector<HLODMaterialBaked>     m_MeshBaked;
        bool                               m_Done = false;
        std::optional<std::string>         m_Error;
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

#pragma once

// A PARTITIONED WORLD IN PLAY: ONLY THE NEIGHBOURHOOD OF THE CAMERA IS IN THE ECS, AND ITS CELLS ARE READ OFF
// THE MAIN THREAD.
//
// The decisions are pure and tested (Serialize/WorldPartitionResidencyExecutor.hpp, suite WorldPartitionStreamer);
// this is the Scene side of them — the ResidencyWorld whose loads read a unit's records on a JobSystem worker
// (Serialize/WorldCellLoader.hpp), whose Activate is SceneSerializer::InstantiateRecords of those records, and
// whose Destroy is Scene::DestroyEntity.
//
// TWO BEGINNINGS, ONE STREAMING.
//
//   Begin — THE EDITOR'S PLAY (analysis A8), and a runtime handed a .desce with no cooked world beside it. The
//   whole world is already entities (Edit keeps it whole, because saving writes the ECS), so the records are
//   the Play snapshot, parsed once, and the cell source is that memory. The editor takes its snapshot BEFORE
//   this starts and Stop rebuilds the scene from it, so what streaming destroyed during Play comes back whole.
//
//   BeginCooked — A GAME. Its world was cut into cell files by the packager (WorldCells.hpp, WP8/WP9); only the
//   index and the always-loaded file are read before the first frame, and every cell is read when the camera
//   first wants it. The start of a world is then the start of a small scene: its always-loaded part.
//
// Either way a cell streamed back in is the cell as it was authored, not as Play left it: a record is not
// written back when its cell leaves — UE's runtime cells have the same contract.

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCellLoader.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;

    // A game's world as its cook left it, read up to the point where a scene can be made from it: the index,
    // the plan residency needs, and the always-loaded part as scene JSON — the scene-wide part and the
    // always-loaded records — which the ordinary loader (SceneSerializer::DeserializeFromJson) takes. What the
    // runtime loads instead of the .desce.
    struct CookedWorldStart
    {
        std::string                             Directory; // WorldCells::CookedWorldDirectory of the scene
        std::shared_ptr<WorldCells::WorldIndex> Index;
        WorldCells::IndexedWorld                Indexed;
        std::string                             AlwaysLoadedJson;
    };

    // Whether @p scenePath has a cooked world beside it (through the VFS: a pak or the disk) and, when it has,
    // that world read up to its always-loaded part. nullopt — and nothing read — when there is no index there;
    // an index that is there but cannot be read, or whose files do not match it, is an error naming the file.
    [[nodiscard]] Common::ResultStr<std::optional<CookedWorldStart>>
    ReadCookedWorld( const std::string& scenePath );

    class WorldStreamer final : public Rules::ResidencyWorld
    {
    public:
        // Starts streaming @p scene, whose entities are the whole of @p snapshotJson right now. Returns nullptr —
        // and touches nothing — when the snapshot states no WorldPartition block. The streaming source is the
        // scene's active camera.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<WorldStreamer>>
        Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson );

        // Starts streaming @p scene, whose entities are exactly @p world's always-loaded records right now (the
        // caller loaded world.AlwaysLoadedJson into it). No cell is an entity yet: the first Tick starts reading
        // the ones the camera wants.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<WorldStreamer>>
        BeginCooked( Scene& scene, Assets::AssetManager& assets, CookedWorldStart world );

        // One frame: collect the reads that finished, step the residency from the active camera's position and
        // apply what it decides.
        [[nodiscard]] Common::BoolResultStr Tick( double nowSeconds );

        // WHAT THE LAST Tick DID, for an instrument that attributes a frame's cost (the editor's --flight).
        // Reset at the start of every Tick, so it never carries an earlier frame's activations.
        struct TickReport
        {
            Rules::ResidencyTick Tick;
            double               ActivationMs = 0.0; // summed over this tick's InstantiateRecords calls
            std::string          ActivatedUnits;     // Rules::DescribeResidencyUnit of each, space-separated
            std::size_t          LiveRecords   = 0;  // records held as entities after the tick
            std::size_t          LoadsInFlight = 0;  // cell reads still on a worker after the tick
        };
        [[nodiscard]] const TickReport& LastTick() const
        {
            return m_LastTick;
        }

        // Whether @p scene is the one this streams: the editor updates several scenes a frame.
        [[nodiscard]] bool Streams( const Scene& scene ) const
        {
            return &scene == m_Scene;
        }

        // Says what streaming cost, once: the activation measurement MsPerRecord comes from.
        ~WorldStreamer() override;
        WorldStreamer( const WorldStreamer& )            = delete;
        WorldStreamer& operator=( const WorldStreamer& ) = delete;

        [[nodiscard]] Common::BoolResultStr Activate( std::size_t                  unit,
                                                      std::span<const std::size_t> records ) override;
        void                                Destroy( std::span<const std::size_t> records ) override;
        void                                StartLoad( std::size_t unit, std::uint64_t ticket ) override;
        void                                CancelLoad( std::size_t unit, std::uint64_t ticket ) override;
        void                                Unload( std::size_t unit ) override;
        [[nodiscard]] std::vector<Rules::LoadOutcome> TakeLoadOutcomes() override;

    private:
        WorldStreamer( Scene& scene, Assets::AssetManager& assets, std::string sceneName );
        [[nodiscard]] Common::ResultStr<Rules::StreamingSource> Source() const;

        Scene*                m_Scene;
        Assets::AssetManager* m_Assets;
        std::string           m_SceneName;
        // Per record index (the executor's): the id its entity carries, which is how Destroy finds it.
        std::vector<Common::UUID>               m_RecordIds;
        std::optional<Rules::ResidencyExecutor> m_Executor;
        // Every streamer Begin/BeginCooked hand out holds an executor; only one abandoned half-built reaches
        // the destructor without it, and the destructor checks for itself. The one unchecked access is here.
        Rules::ResidencyExecutor& Executor()
        {
            return *m_Executor; // NOLINT(bugprone-unchecked-optional-access)
        }
        // What the cell source reads: the Play snapshot's records (Begin) or the cooked world's index
        // (BeginCooked). Shared with the source, which the loader shares with its workers.
        std::shared_ptr<const SceneSerialized>        m_Snapshot;
        std::shared_ptr<const WorldCells::WorldIndex> m_Index;
        std::unique_ptr<WorldCellLoader>              m_Loader;

        // What activation actually cost, measured around InstantiateRecords.
        std::size_t m_Activations      = 0;
        std::size_t m_RecordsActivated = 0;
        double      m_ActivationMs     = 0.0;
        double      m_WorstUnitMs      = 0.0;
        std::size_t m_MostResident     = 0;

        TickReport m_LastTick;
    };
} // namespace Desert::Core

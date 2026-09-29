#pragma once

// A PARTITIONED WORLD IN PLAY: ONLY THE NEIGHBOURHOOD OF ITS STREAMING SOURCES IS IN THE ECS, AND ITS CELLS ARE
// READ OFF THE MAIN THREAD.
//
// THE SOURCES (WP24, UE's UWorldPartitionStreamingSourceComponent). Every entity with an enabled
// ECS::StreamingSourceComponent, the player's pawn among them (Core::SpawnDefaultPawn gives it one), plus
// whatever an instrument hands Begin/Tick (the editor's --flight camera — UE's streaming source providers).
// Residency follows their UNION. The view is not a source: a camera looking at the world is not where the
// world must exist. A Play with no source at all streams nothing beyond the always-loaded part and says so
// once, by name — the origin or the camera standing in would stream the wrong neighbourhood in silence.
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
//   index and the always-loaded file are read before the first frame, and every cell is read when a source
//   first wants it. The start of a world is then the start of a small scene: its always-loaded part.
//
// Either way a cell streamed back in is the cell as it was authored, not as Play left it: a record is not
// written back when its cell leaves — UE's runtime cells have the same contract.
//
// THE HLODs (WP11). Every cell that draws something has an Instancing HLOD: the cook's HLOD file (BeginCooked)
// or the same builder run over the snapshot (Begin). Its records are entities for the whole session, made
// before the first frame, and only their Visibility changes — in the same Tick that activates or destroys the
// cell (Serialize/WorldPartitionHLODSwitch.hpp), so a cell and its HLOD never both draw and never both miss.

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCellLoader.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODSwitch.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp>

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>
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

    class WorldStreamer final : public Rules::ResidencyWorld, public Rules::HLODVisibilityWorld
    {
    public:
        // Starts streaming @p scene, whose entities are the whole of @p snapshotJson right now. Returns nullptr —
        // and touches nothing — when the snapshot states no WorldPartition block. The sources are the scene's
        // (see THE SOURCES) plus @p instruments; the pawn must already be spawned (Core::BeginPlay first).
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<WorldStreamer>>
        Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson,
               std::span<const Rules::StreamingSource> instruments = {} );

        // Starts streaming @p scene, whose entities are exactly @p world's always-loaded records right now (the
        // caller loaded world.AlwaysLoadedJson into it). No cell is an entity yet: the first Tick starts reading
        // the ones the sources want.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<WorldStreamer>>
        BeginCooked( Scene& scene, Assets::AssetManager& assets, CookedWorldStart world );

        // One frame: collect the reads that finished, step the residency from the sources (the scene's and
        // @p instruments) and apply what it decides.
        [[nodiscard]] Common::BoolResultStr Tick( double                                  nowSeconds,
                                                  std::span<const Rules::StreamingSource> instruments = {} );

        // WHAT THE LAST Tick DID, for an instrument that attributes a frame's cost (the editor's --flight).
        // Reset at the start of every Tick, so it never carries an earlier frame's activations.
        struct TickReport
        {
            Rules::ResidencyTick Tick;
            double               ActivationMs = 0.0; // summed over this tick's InstantiateRecords calls
            std::string          ActivatedUnits;     // Rules::DescribeResidencyUnit of each, space-separated
            std::size_t          LiveRecords   = 0;  // records held as entities after the tick
            std::size_t          LoadsInFlight = 0;  // cell reads still on a worker after the tick
            // Whether streaming keeps up, and whether the game must wait (WorldPartitionStreamingPerformance.hpp).
            Rules::StreamingAssessment Streaming;
        };

        // True while a cell under the streaming source is not resident: the host stops gameplay time and
        // draws the loading overlay, and keeps calling Tick — the loader reads on its workers meanwhile.
        [[nodiscard]] bool BlocksPlay() const
        {
            return m_LastTick.Streaming.Blocks();
        }

#if DESERT_DEV_INSTRUMENTS
        // DEV ONLY: every cell read is reported @p ticks Ticks late, so a flight can outrun streaming on
        // purpose (the editor's --stream-delay-ticks). Ticks and not milliseconds, so a capture is reproducible.
        void SetDebugLoadDelayTicks( std::uint32_t ticks );
#endif
        [[nodiscard]] const TickReport& LastTick() const
        {
            return m_LastTick;
        }

        // Whether @p scene is the one this streams: the editor updates several scenes a frame.
        [[nodiscard]] bool Streams( const Scene& scene ) const
        {
            return &scene == m_Scene;
        }

        // WHAT THE EDITOR'S WORLD PARTITION PANEL PAINTS IN PLAY (Editor/Panels/WorldPartition): the cells,
        // each unit's residency, the settings that turn a source's position into its loading circle, and
        // where the source was on the last Tick. Read-only views of the executor's own state, not copies.
        [[nodiscard]] const Rules::WorldPartitionPlan& Plan() const
        {
            return m_Executor->Plan(); // NOLINT(bugprone-unchecked-optional-access): see Executor() below
        }
        [[nodiscard]] const Rules::ResidencyState& Residency() const
        {
            return m_Executor->State(); // NOLINT(bugprone-unchecked-optional-access): see Executor() below
        }
        [[nodiscard]] const WorldPartitionSerialized& Partition() const
        {
            return m_Partition;
        }
        [[nodiscard]] const Rules::ResidencySettings& Settings() const
        {
            return m_Settings;
        }
        // The sources the last Begin/Tick streamed around; empty before BeginCooked's first Tick and in a Play
        // with no source.
        [[nodiscard]] const std::vector<Rules::StreamingSource>& LastSources() const
        {
            return m_LastSources;
        }

        // Every enabled StreamingSourceComponent of @p scene, at its entity's world position, then @p instruments.
        // A disabled one is not a source; an override range is the component's own.
        [[nodiscard]] static std::vector<Rules::StreamingSource>
        GatherSources( Scene& scene, std::span<const Rules::StreamingSource> instruments );

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
        void                                          SetHLODVisible( std::size_t hlod, bool visible ) override;

        // What stands in for the cells that are not drawn: nullopt only on a streamer that failed to begin.
        [[nodiscard]] const std::optional<Rules::HLODRuntime>& HLODs() const
        {
            return m_HLODs;
        }

    private:
        WorldStreamer( Scene& scene, Assets::AssetManager& assets, std::string sceneName );
        // Takes @p sources as this frame's; the first frame with none logs the one named error (THE SOURCES).
        void UseSources( std::vector<Rules::StreamingSource> sources );

        // One HLOD as the streamer takes it: the cell unit it stands in for and its records.
        struct CellHLOD
        {
            std::size_t                     Unit = 0;
            std::vector<Assets::EntityData> Records;
        };
        // Makes every HLOD's records entities, then shows the ones whose cell is not visible now — before the
        // first frame, so the first frame already has no hole and no double. @p holes are said once, by reason.
        [[nodiscard]] Common::BoolResultStr BeginHLODs( std::vector<CellHLOD>                 hlods,
                                                        std::span<const Rules::HLODExclusion> holes );

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
        std::uint32_t m_FramesWaiting = 0;

        // Per HLOD (the switch's index): the ids of its entities, which is how SetHLODVisible finds them.
        std::vector<std::vector<Common::UUID>> m_HLODIds;
        std::optional<Rules::HLODRuntime>      m_HLODs;

        // The one settings value both beginnings hand the executor, kept so the panel reads the same margin.
        Rules::ResidencySettings              m_Settings;
        WorldPartitionSerialized              m_Partition;
        std::vector<Rules::StreamingSource>   m_LastSources;
        bool                                  m_SaidNoSource = false;
    };
} // namespace Desert::Core

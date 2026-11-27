#pragma once

// A PARTITIONED WORLD IN PLAY: ONLY THE NEIGHBOURHOOD OF ITS STREAMING SOURCES IS IN THE ECS, AND ITS CELLS ARE
// READ OFF THE MAIN THREAD.
//
// THE SOURCES (WP24, WP-STREAM-SOURCE-VIEW; UE's UWorldPartitionStreamingSourceComponent and providers). The
// player is a source WHERE IT LOOKS FROM (Core::PlayerStreamingViewOf, PlayerStreamingView.hpp): UE's
// APlayerController is a streaming source provider located at GetPlayerViewPoint, so the view target camera - the
// pawn's, an Auto Activate camera, Play from Here's, a camera a scenario pins - streams the world it looks at,
// pawn or none. Beside it every entity with an enabled ECS::StreamingSourceComponent - a pawn only when its prefab
// authors one (Pawn_Player.deprefab: the ground under the body while the view is elsewhere), as UE's character
// carries its own - the scripts' providers, and whatever an instrument hands Begin/Tick (the editor's --flight
// camera). Residency follows their UNION. No other camera is a source: the editor's view in Edit, a preview, a
// Sequencer camera that is not the player's view.
//
// A TELEPORT (kStreamingTeleportDistanceCm) resets the jumped source's velocity and holds the cells it left
// until the cell under it is resident: the destination is made before the origin is let go (see Tick).
// A Play with no source at all streams nothing beyond the always-loaded part and says so once, by name — the
// origin standing in would stream the wrong neighbourhood in silence.
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
//
// THE HLOD CELLS (WP-FAR-8). When a world's HLOD layer is spatially loaded (CellSize, LoadingRange), its HLODs
// are not entities for the session: they live in HLOD cells streamed by a SECOND ResidencyExecutor over the
// hierarchy's plan (Serialize/WorldPartitionHLODHierarchy.hpp) - read on workers, activated under the budget,
// destroyed the frame they leave range - and only the top of each chain (an always-loaded layer) is made before
// the first frame, one HLOD at a time. A city then holds the HLODs around its sources and one coarse top, not
// every HLOD it has. Both beginnings: Play reads the saved HLODs, a game the cooked HLOD cells (WorldCells.hpp,
// THE HLOD CELLS OF A SPATIALLY LOADED LAYER) through the same region chunks as its world cells.

#include <set>
#include <Engine/Core/Serialize/WorldPartitionFormat.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/WorldCellLoader.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/CookedWorld.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODSwitch.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp>

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/ResultStr.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace Desert::Core
{
    struct SceneSerialized;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;

    class WorldStreamer final : public Rules::ResidencyWorld, public Rules::HLODVisibilityWorld
    {
    public:
        // Starts streaming @p scene, whose entities are the whole of @p snapshotJson right now. Returns nullptr —
        // and touches nothing — when the snapshot states no WorldPartition block. The sources are the scene's
        // (see THE SOURCES) plus @p instruments; the pawn must already be spawned (Core::BeginPlay first).
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<WorldStreamer>>
        Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson,
               const std::filesystem::path& scenePath, // the saved scene (its built HLODs); empty = unsaved

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

        // RUNTIME DATA LAYERS (WP-DATA-LAYERS-RUNTIME; UE UDataLayerManager). Every Runtime layer of the world starts
        // Play and a packaged game in its InitialRuntimeState; Set moves it (UE SetDataLayerInstanceRuntimeState) and
        // the next Tick loads, activates, deactivates or releases its cells (Rules::StepResidency). Refused, by name,
        // for a layer the world does not state or an Editor layer. Luau reaches these through WP-SCRIPT-API.
        [[nodiscard]] Common::BoolResultStr SetDataLayerRuntimeState( std::string_view layer, DataLayerRuntimeState state );
        // UE GetDataLayerInstanceRuntimeState: nullopt for a name the world does not state or an Editor layer.
        [[nodiscard]] std::optional<DataLayerRuntimeState> GetDataLayerRuntimeState( std::string_view layer ) const;
        // UE GetCellEffectiveWantedState: how far its runtime layers want unit @p unit (Activated for none).
        [[nodiscard]] DataLayerRuntimeState GetUnitEffectiveRuntimeState( std::size_t unit ) const;

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

#if DESERT_DEV_INSTRUMENTS
        // DEV ONLY - what one unit's last read and activation took (the gameplay debugger's World Partition
        // category; UE wp.Runtime.ToggleDrawRuntimeCellsDetails). Wall milliseconds; negative = not measured yet.
        struct UnitTiming
        {
            double LoadMs       = -1.0; // StartLoad to the Tick that collected the read: queue and worker
            double ActivationMs = -1.0; // its last InstantiateRecords on the main thread
        };
        // nullptr for a unit never read by this streamer.
        [[nodiscard]] const UnitTiming* TimingOf( std::size_t unit ) const
        {
            return unit < m_UnitTimings.size() ? &m_UnitTimings[unit] : nullptr;
        }
#endif

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
        // The id the entity of executor record @p record carries (Rules::ResidencyUnitMembers names records).
        [[nodiscard]] const Common::UUID& RecordId( std::size_t record ) const
        {
            return m_RecordIds.at( record );
        }
        // THE NETWORK'S WORLD DELTA (NET-ARCH §3.3, UE DestroyedStartupOrDormantActors): stable records the server
        // destroyed. Their entities (and their child records') are destroyed now if resident, and from then on
        // Activate never makes them, whichever cell brings them back. For the session.
        void DestroyRecordsForNet( std::span<const std::uint64_t> ids );
        // THE NETWORK'S VISIBILITY GATE (UE ServerUpdateLevelVisibility MakingVisible +
        // ClientAckUpdateLevelVisibility): while held, a cell unit is made hidden as always but revealed only once
        // the server has acknowledged it - the acknowledgement carries the cell's world delta, applied first, so a
        // record the server destroyed is never seen for a frame. Always-loaded units are not gated. An unload
        // forgets the acknowledgement: the cell's next load is acknowledged again. Released (everything reveals as
        // usual) when the session closes. While held, the only streaming sources are the client's own player's:
        // its view and the components of its pawn and the pawn's descendants - see Tick.
        void SetNetRevealHold( bool hold );
        void AcknowledgeUnitsForNet( std::span<const std::uint32_t> units );
        [[nodiscard]] const WorldPartitionSerialized& Partition() const
        {
            return m_Partition;
        }
        [[nodiscard]] const Rules::ResidencySettings& Settings() const
        {
            return m_Settings;
        }
        // The sources the last Begin/Tick streamed around; empty before BeginCooked's first Tick and in a Play
        // with no source. The origins a teleport holds (see THE SOURCES) are not among them.
        [[nodiscard]] const std::vector<Rules::StreamingSource>& LastSources() const
        {
            return m_LastSources;
        }
        // The keys of the last Tick's first LastSources(): a component source's entity (entt id), or
        // kPlayerViewSourceKey for the player's view (PlayerStreamingView.hpp). The sources past them - the
        // scripts' providers and the instruments - have none. Empty before the first Tick.
        [[nodiscard]] const std::vector<std::uint32_t>& LastSourceKeys() const
        {
            return m_LastSourceKeys;
        }

        // Every enabled StreamingSourceComponent of @p scene, at its entity's world position and facing its
        // forward, with the component's one shape; then the player's view (PlayerViewStreamingSource) whenever
        // Play has one (see THE SOURCES); then the scripts' providers; then @p instruments. A disabled one is not
        // a source; an override range is the shape's own radius. @p entities, when given, receives the key of each
        // leading source, in order: each component source's entity, then kPlayerViewSourceKey for the view.
        // Velocity is zero here: only a Tick knows the time between two positions.
        // A sector with Angle From Camera is as wide as its entity's camera sees across (CameraViewAngle);
        // @p withoutCamera, when given, receives each such entity that has no camera to read (its source keeps
        // the authored Sector Angle, and the Tick names it).
        [[nodiscard]] static std::vector<Rules::StreamingSource>
        GatherSources( Scene& scene, std::span<const Rules::StreamingSource> instruments,
                       std::vector<std::uint32_t>* entities      = nullptr,
                       std::vector<std::uint32_t>* withoutCamera = nullptr );

        // The horizontal angle, degrees, a perspective camera of vertical @p verticalFovDegrees sees across a
        // viewport of width over height @p aspect: 2 atan(tan(v / 2) * aspect), capped at the full circle.
        [[nodiscard]] static float CameraViewAngle( float verticalFovDegrees, float aspect );

        // Says what streaming cost, once: the activations, their slices, and what each record kind measured.
        ~WorldStreamer() override;
        WorldStreamer( const WorldStreamer& )            = delete;
        WorldStreamer& operator=( const WorldStreamer& ) = delete;

        [[nodiscard]] Common::BoolResultStr           Activate( std::size_t unit, std::size_t firstMember,
                                                                std::span<const std::size_t> records ) override;
        [[nodiscard]] Common::ResultStr<bool>         FinishActivation( std::size_t                  unit,
                                                                        std::span<const std::size_t> records ) override;
        [[nodiscard]] Rules::ActivationRecordKind     ActivationKindOf( std::size_t unit,
                                                                        std::size_t member ) const override;
        void                                Destroy( std::span<const std::size_t> records ) override;
        void                                StartLoad( std::size_t unit, std::uint64_t ticket ) override;
        void                                CancelLoad( std::size_t unit, std::uint64_t ticket ) override;
        void                                Unload( std::size_t unit ) override;
        [[nodiscard]] std::vector<Rules::LoadOutcome> TakeLoadOutcomes() override;
        void                                          SetHLODVisible( std::size_t hlod, bool visible ) override;
        // THE HLOD WARMUP (Serialize/WorldPartitionHLODSwitch.hpp, THE WARMUP): what a cell's read records or an
        // HLOD's own records name, as their dependency closure - read on workers, pinned against the eviction
        // sweep, its textures asked for at the size every view measures the cell's bounds at - until resident.
        [[nodiscard]] bool WarmCell( std::size_t unit ) override;
        [[nodiscard]] bool WarmHLOD( std::size_t hlod ) override;
        [[nodiscard]] bool WarmHLODCell( std::size_t unit ) override;
        void               EndCellWarmup( std::size_t unit ) override;
        void               EndHLODWarmup( std::size_t hlod ) override;
        void               EndHLODCellWarmup( std::size_t unit ) override;

        // What stands in for the cells that are not drawn: nullopt only on a streamer that failed to begin.
        [[nodiscard]] const std::optional<Rules::HLODRuntime>& HLODs() const
        {
            return m_HLODs;
        }

    private:
        WorldStreamer( Scene& scene, Assets::AssetManager& assets, std::string sceneName );
        // Takes @p sources as this frame's; the first frame with none logs the one named error (THE SOURCES).
        void UseSources( std::vector<Rules::StreamingSource> sources );
        // Compares each unit's residency with what the last call saw and sets WorldPartitionStreamingEvents.
        void PublishStreamingEvents();
        // A keyed source that moved kStreamingTeleportDistanceCm or more since the last Tick: a teleport.
        struct SourceJump
        {
            std::uint32_t Key   = 0; // entt id, or kPlayerViewSourceKey
            std::size_t   Index = 0; // into this Tick's sources
            glm::vec3     From{ 0.0f };
            float         Distance = 0.0f; // cm
        };
        // Gives each of the first @p keys.size() sources the velocity it moved at since the last Tick, by key;
        // a teleport gets velocity 0 and is returned.
        [[nodiscard]] std::vector<SourceJump> StampVelocities( std::vector<Rules::StreamingSource>& sources,
                                                               std::span<const std::uint32_t>       keys,
                                                               double                               nowSeconds );
        // The resident cells @p origins want that no source keeps - outside every source's range grown by
        // UnloadMargin, the band StepResidency keeps anyway: what the hold alone keeps. A refused query is an error.
        [[nodiscard]] Common::ResultStr<std::size_t>
        CellsHeldByTeleport( std::span<const Rules::StreamingSource> origins ) const;

        // One HLOD as the streamer takes it: the cell unit it stands in for and the read of its records.
        // A READ, NOT THE RECORDS (WP22-LIT): a landscape HLOD record carries its built mesh inline (four
        // 65 536-vertex tiles, 84 MB of text per G0L5 file), and holding every HLOD's parsed records until all
        // of them were entities put ~2 GB on top of the world the editor held - Play from Here after Load All
        // was killed at 6146 MB. BeginHLODs reads one HLOD, makes it entities and drops its records before the
        // next is read (UE streams HLOD cells the same way: one cell's package at a time).
        struct CellHLOD
        {
            std::size_t                                                         Unit = 0;
            std::function<Common::ResultStr<std::vector<Assets::EntityData>>()> Read;
        };
        // Makes every HLOD's records entities, one HLOD at a time, then shows the ones whose cell is not visible
        // now — before the first frame, so the first frame already has no hole and no double. @p holes are said
        // once, by reason.
        [[nodiscard]] Common::BoolResultStr BeginHLODs( std::vector<CellHLOD>                 hlods,
                                                        std::span<const Rules::HLODExclusion> holes );
        // THE HLOD CELLS (see the top): @p residency is MakeHLODResidency's of @p hierarchy, @p nodeIds per node
        // the ids of its records (empty for one with none) and @p source reads one HLOD residency unit's records -
        // the saved HLODs (Begin) or the cooked HLOD cells (BeginCooked). Makes the always-loaded nodes entities
        // one at a time, starts the HLOD residency and shows what stands in now.
        [[nodiscard]] Common::BoolResultStr BeginHLODCells( Rules::HLODHierarchy                   hierarchy,
                                                            Rules::HLODResidency                   residency,
                                                            std::vector<std::vector<Common::UUID>> nodeIds,
                                                            std::shared_ptr<const Rules::WorldCellSource> source,
                                                            std::span<const Rules::HLODExclusion>         holes );

        // The ResidencyWorld of the HLOD cells: their reads, activations and destructions (WorldStreamer.cpp).
        class HLODCellWorld;

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

        // What activation actually cost, measured around each slice's InstantiateRecords and its bodies.
        std::size_t m_Activations      = 0; // units completed
        std::size_t m_Slices           = 0;
        std::size_t m_RecordsActivated = 0;
        double      m_ActivationMs     = 0.0;
        double      m_WorstSliceMs     = 0.0;
        double      m_WorstUnitMs      = 0.0; // a unit's slices and finish, summed over its frames
        // Per Activating unit: the entities its slices made, hidden until it is whole (THE SLICES).
        std::unordered_map<std::size_t, std::vector<std::uint32_t>> m_Made;
        // DestroyRecordsForNet's ids, and the child records Activate skipped with them.
        std::unordered_set<std::uint64_t> m_NetDestroyed;
        bool                              m_NetRevealHold = false;
        bool                              m_SaidNetOwnSources = false; // "streams from its own player only", once
        std::unordered_set<std::size_t>   m_NetAcknowledged; // cell units the server acknowledged (gate above)
        std::size_t m_MostResident     = 0;

        TickReport m_LastTick;
#if DESERT_DEV_INSTRUMENTS
        // Per unit (index = unit, grown as units are read): TimingOf's answers, and when each read started.
        std::vector<UnitTiming>                            m_UnitTimings;
        std::vector<std::chrono::steady_clock::time_point> m_LoadStarted;
#endif
        std::uint32_t m_FramesWaiting = 0;
        // True while the last Tick was Critical with blocking disabled (logged once per such stretch).
        bool m_SaidCriticalUnblocked = false;

        // Per HLOD (the switch's index): the ids of its entities, which is how SetHLODVisible finds them.
        std::vector<std::vector<Common::UUID>> m_HLODIds;
        // THE IMPOSTOR PAIR (IMPOSTORS-HLOD): the HLOD record ids an Impostor layer built as cards and as their
        // instanced source meshes (WorldCells ImpostorCards / ImpostorFallbacks). A shown HLOD draws the cards
        // while m_HLODImpostors (Scalability ViewDistance.HLODImpostors, read every Tick) is on, the fallbacks
        // while it is off; every other HLOD record is drawn whenever its HLOD is.
        std::unordered_set<std::uint64_t> m_ImpostorCards;
        std::unordered_set<std::uint64_t> m_ImpostorFallbacks;
        bool                              m_HLODImpostors = true;
        // States @p cards / @p fallbacks of one saved or cooked HLOD (any of the four HLOD rows).
        void NoteImpostorPair( std::span<const std::uint64_t> cards, std::span<const std::uint64_t> fallbacks );
        // Reads the machine's ViewDistance.HLODImpostors; on a change re-shows every shown HLOD with the other side.
        void SyncHLODImpostors();
        std::optional<Rules::HLODRuntime>      m_HLODs;
        // A streamed hierarchy only (BeginHLODCells): the hierarchy, its residency, the executor that streams its
        // cells and the world that executor acts on.
        std::optional<Rules::HLODHierarchy>     m_HLODHierarchy;
        std::optional<Rules::HLODResidency>     m_HLODResidency;
        std::optional<Rules::ResidencyExecutor> m_HLODExecutor;
        std::unique_ptr<HLODCellWorld>          m_HLODCells;
        // False through BeginHLODs' first Sync (which sets every HLOD); after it each switch is logged.
        bool m_HLODSwitchesSaid = false;
        // THE WARMUPS IN FLIGHT (WorldStreamer.cpp): by world unit (a cell to be shown) and by HLOD (an HLOD to be
        // shown in its cell's place). What each HLOD's records name is gathered when they are read: per HLOD made
        // by BeginHLODs, per HLOD residency unit made at the start of BeginHLODCells (a streamed HLOD cell's come
        // with its read, HLODCellWorld).
        struct Warming;
        [[nodiscard]] bool WarmUp( std::map<std::size_t, std::unique_ptr<Warming>>& warming, std::size_t key,
                                   const Rules::CellBounds& bounds, const std::string& what,
                                   const std::vector<std::string>* guids );
        void               PublishWarmups();
        // Whether world unit @p unit's square holds a streaming source: Play waits for that cell, so it is never
        // held for a warmup (WorldPartitionStreamingPerformance.hpp, CRITICAL).
        [[nodiscard]] bool                                        UnderSource( std::size_t unit );
        std::map<std::size_t, std::unique_ptr<Warming>>           m_WarmCells;
        std::map<std::size_t, std::unique_ptr<Warming>>           m_WarmHLODs;
        std::map<std::size_t, std::unique_ptr<Warming>>           m_WarmHLODCells; // by HLOD residency unit
        std::vector<std::vector<std::string>>                     m_HLODDependencies;
        std::unordered_map<std::size_t, std::vector<std::string>> m_HLODUnitDependencies;

        // The one settings value both beginnings hand the executor, kept so the panel reads the same margin.
        Rules::ResidencySettings              m_Settings;
        WorldPartitionSerialized              m_Partition;
        std::vector<Rules::StreamingSource>   m_LastSources;
        std::vector<std::uint32_t>            m_LastSourceKeys; // LastSourceKeys()
        bool                                  m_SaidNoSource = false;
        // "the player's view is a streaming source", said when the view source appears; cleared while it is gone.
        bool m_SaidPlayerView = false;
        // THE TELEPORT HOLD (see THE SOURCES): where each source that jumped stood before the hold, as a source
        // of its own (unmoving, last in the order) that the residency steps over beside the sources, until a Tick
        // that does not wait for the cell under a source; one origin per source key (m_TeleportOriginKeys, same
        // index) - a held source's later jumps add none; the frames it waited and the cells it said.
        std::vector<Rules::StreamingSource> m_TeleportOrigins;
        std::vector<std::uint32_t>          m_TeleportOriginKeys;
        std::uint32_t                       m_TeleportHoldFrames = 0;
        std::size_t                         m_TeleportHeldSaid   = 0;
        // Per unit of runtime data layers: the effective state and residency last said in the log (SayDataLayerUnits).
        std::vector<std::uint8_t> m_SaidLayerUnit;
        void                      SayDataLayerUnits();
        // Entities whose Angle From Camera found no camera, already named in the log.
        std::unordered_set<std::uint32_t> m_SaidWithoutCamera;
        // Where each keyed source stood on the last Tick, and when: what its velocity is measured from.
        std::unordered_map<std::uint32_t, glm::vec3> m_SourcePositions;
        double                                       m_SourcesAt = 0.0;
        // What each unit's residency was when PublishStreamingEvents last looked, and its publication count.
        std::vector<Rules::Residency> m_StatesSeen;
        std::uint64_t                 m_EventPublication = 0;
    };

    // THE STREAMER OF A SCENE, ON THE SCENE'S REGISTRY CONTEXT (UE: a world's UWorldPartitionSubsystem): set while
    // a WorldStreamer streams the scene - by Begin / BeginCooked as soon as its executor exists, so a script asks
    // before the first tick and is told the truth (nothing has arrived) - and removed by its destructor. What the
    // WorldPartition script library (Libraries/WorldPartitionLibrary.hpp) reads the plan and the residency
    // through; a host draws from WorldStreamingWait instead.
    struct WorldStreamerOfScene
    {
        WorldStreamer* Streamer = nullptr;
    };

    // STREAMING SOURCES NO ENTITY CARRIES (UE IWorldPartitionStreamingSourceProvider, registered with the World
    // Partition subsystem): a script states one by name (WorldPartition.provideSource) and it streams until the
    // script removes it. On the scene's registry context; GatherSources reads them after the component sources.
    struct ProvidedStreamingSources
    {
        std::map<std::string, Rules::StreamingSource> Sources;
    };
} // namespace Desert::Core

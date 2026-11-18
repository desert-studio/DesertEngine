#pragma once

#include <array>

// THE WORLD PARTITION MAP — UE's World Partition editor window (SWorldPartitionEditorGrid2D), as an ImGui panel.
//
// A top-down map of the partition: every cell the plan holds, coloured by what streaming is doing with it, the
// background grid of the level that is legible at this zoom, the world axes, and the streaming source with its
// loading circle and unload band. What decides the picture — view transform, zoom, paint order, the cell under
// the cursor, colours — is WorldPartitionMap.hpp and tested there; this file only reads the data and paints it.
//
// TWO SOURCES OF DATA, ONE PICTURE.
//   Edit — the plan of the scene as it is now, made the way the loader makes it (SceneSerializer → the parsed
//          file → PlanWorldPartition). Made when the panel first shows a scene and on Refresh, not every frame:
//          serialising a world is a frame's worth of work. No cell streams, so every cell is Unstreamed.
//   Play — the streamer's own plan and residency (WorldStreamer::Plan / Residency), read live each frame, and
//          its last streaming source. The streamer is asked for through a getter each frame rather than
//          handed in once, because Play's stop and a streaming error both destroy it from the editor's side.

#include <Editor/Core/IPanel.hpp>
#include "HLODMaterialBake.hpp"
#include "LandscapeHLODBake.hpp"
#include "WorldPartitionMap.hpp"

#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
    class WorldStreamer;
} // namespace Desert::Core

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class WorldPartitionPanel final : public IPanel
    {
    public:
        // @p streamer answers the editor's live streamer, or nullptr outside Play.
        using StreamerGetter = std::function<const ::Desert::Core::WorldStreamer*()>;

        WorldPartitionPanel( std::shared_ptr<::Desert::Core::Scene> scene, ::Desert::Assets::AssetManager* assets,
                             StreamerGetter streamer );

        void OnUIRender() override;
        void SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene ) override;

        /// SWITCHES THIS PANEL'S SCENE ON AS A PARTITIONED WORLD — UE's "Convert Level to World
        /// Partition", as one undoable history entry.
        ///
        /// The decision is Rules::ConvertToWorldPartition (pure, tested in
        /// Desert/Tests/Engine/WorldPartition); this only stores its answer on the scene, pushes the
        /// entry that can put it back, and marks the plan for a rebuild. The scene becomes dirty because
        /// the entry moves CommandHistory's revision, which is what the editor's unsaved-changes star
        /// reads — there is no second dirty flag to set.
        ///
        /// REFUSES, BY NAME, a scene that is already partitioned. The refusal is the returned error, so
        /// the button and the command palette both show the same sentence.
        [[nodiscard]] Common::BoolResultStr ConvertSceneToWorldPartition();

        /// UE's Build > Build HLODs (WP-FAR-6B, lead D1): every cell's HLOD of the world SAVED at the scene's
        /// file (scene + entity files, as WorldCook reads it), built by Core::HLODBuilders and written to
        /// `<Scene>_HLOD/` (WorldCells::BuiltHLODWriter, cell by cell); read back and checked stale-free. Refused,
        /// by name, for a scene not opened from a file or a world the build refuses. Two phases: this call checks
        /// the world and starts the landscape tiles' BaseColor bake (LandscapeHLODBake, over the next editor
        /// frames); TickBuild finishes it — the HLODs are built with each landscape HLOD bound to its baked
        /// material, written and read back. A world without landscape HLODs is built here at once.
        [[nodiscard]] Common::BoolResultStr BuildHLODs();

        /// Advances a running Build HLODs (its bake) by one editor frame; called every frame by EditorLayer,
        /// whether the panel's window is open or not.
        void TickBuild();

        /// A Build HLODs is running: its landscape bake has not finished, so its cell files are not written yet.
        [[nodiscard]] bool IsBuildingHLODs() const noexcept
        {
            return m_Bake != nullptr || m_MeshBake != nullptr;
        }

        /// What a region change asks for: the regions to hold, or the whole world.
        struct RegionRequest
        {
            std::vector<::Desert::Core::Rules::CellBounds> Regions; // empty: the always-loaded part only
            bool                                           Whole = false;
        };
        /// The map's Load Region / "Unload All" / "Load All" (WP19-20, decision O3; WP-LOADER-ADAPTERS): Regions
        /// (X/Z, cm) each join the loaders as one more rectangle, switched on (one already listed is switched on
        /// again); Whole switches Load All on; neither is Unload All - every loader off but the pins, the
        /// rectangles and entity lists removed. The scene then holds the union of the loaders (Reload,
        /// EditorRegions::LoadHeld). Refused by name (a scene not opened from its files). An entity that differs
        /// from its file and would leave opens the Save / Discard / Cancel prompt (DrawUnloadPrompt) and nothing
        /// changes until it is answered (WP-OUTLINER). The undo history is reset on success, as UE resets its
        /// transactions when it unloads actors: an entry may name an entity that is gone.
        [[nodiscard]] Common::BoolResultStr LoadRegion( const RegionRequest& request );
        /// UE's Scene Outliner Pinned column (the pinned actors loader adapter): whether this user pinned the
        /// world's record @p record, and pinning / unpinning it - the editor holds the world again with the record
        /// held whatever the regions (or left to them), and keeps it for the next open (EditorLoadedRegions). An
        /// unpin that would unload an unsaved entity asks first, as any region change. Refused during Play: the
        /// played world is the whole world, not what the editor holds.
        [[nodiscard]] bool                  IsPinned( std::uint64_t record );
        [[nodiscard]] Common::BoolResultStr SetPinned( std::uint64_t record, bool pinned );
        /// Every record of @p records pinned or unpinned in one change (one reload, one prompt).
        [[nodiscard]] Common::BoolResultStr SetPinned( std::span<const std::uint64_t> records, bool pinned );
        /// Whether Play runs the window's scene (its streamer exists): the editor's loaded part, its pins and its
        /// unsaved state are Edit's, and the outliner's World Partition mode is off.
        [[nodiscard]] bool Playing() const
        {
            return m_Streamer && m_Scene && m_Streamer() != nullptr;
        }
        /// UE's save prompt before an unload (FEditorFileUtils::SaveDirtyPackages): "N unsaved entities: <names>"
        /// with Save (the open scene's save, then the change), Discard (the change, their edits dropped) and
        /// Cancel (nothing, the regions / pins / layers as they were). Drawn every UI frame by EditorLayer,
        /// whether this window is open or not - the outliner's unpin and the palette ask through it too.
        void DrawUnloadPrompt();
        /// THE editor's save of the open scene (SceneFiles::SaveOpenScene), the prompt's Save.
        void SetSaveOpenScene( std::function<bool()> save )
        {
            m_SaveOpenScene = std::move( save );
        }
        // One combo per grid: the layer its cells' HLOD is built with (UE FRuntimePartitionDesc::HLODSetups);
        // undoable.
        void DrawRuntimeGrids();
        void DrawGridHLODLayers();
        /// UE's Data Layer Outliner (WP-DATA-LAYERS): the world's layers - a new one, Initially Loaded (the world's,
        /// undoable through EditWorldPartition), Loaded (this user's, reloading what the editor holds), and Delete,
        /// refused while an entity of the world belongs to the layer.
        void DrawDataLayers();
        /// UE's Data Layer Outliner "Loaded" toggle for this user: the editor holds the world again under the
        /// changed layer state (the same regions or whole world it holds now) and keeps it for the next open
        /// (EditorLoadedRegions). Refused by name for a layer the world does not state, or when the reload is
        /// refused (a dirty entity that would leave) - the layer state is then as it was.
        [[nodiscard]] Common::BoolResultStr SetDataLayerLoaded( const std::string& name, bool loaded );
        /// The names of this panel's world's data layers, in the world's order (the palette's per-layer entries).
        [[nodiscard]] std::vector<std::string> DataLayerNames() const;
        /// The panel's Load Region button: the rectangle selected on the map. Refused by name when nothing is
        /// selected; otherwise LoadRegion. The palette offers it beside Unload All / Load All.
        [[nodiscard]] Common::BoolResultStr LoadSelectedRegion();

        // ── THE LOADERS (WP-LOADER-ADAPTERS; UE's World Partition editor and its loader adapters) ──
        // Every one goes through Reload: refused by name, changing nothing, as LoadRegion is. Each reads the world
        // on disk again first (RebuildEditPlan), so a volume or entity saved a moment ago is known.

        /// UE's Place Actors > Location Volume: a new entity "Location Volume" with a LocationVolumeComponent at
        /// the world origin, selected, one undo step. Its box loads once the scene is saved (the box is its
        /// record's). Refused by name for a scene that is not a partitioned world.
        [[nodiscard]] Common::BoolResultStr AddLocationVolume();
        /// The tags of the scene's Location Volumes, in registry order (the palette's per-volume entries).
        [[nodiscard]] std::vector<std::string> LocationVolumeNames() const;
        /// UE's Location Volume Load / Unload: the saved volume tagged @p name loaded or not for this user.
        /// Refused by name when no saved volume, or more than one, carries the tag.
        [[nodiscard]] Common::BoolResultStr SetLocationVolumeLoaded( const std::string& name, bool loaded );
        /// UE's "Load Region from Selection": a new entity list of every record around the selected entities
        /// (EditorRegions::RecordsAroundSelection). Refused by name when none of them is a placed record of the
        /// saved world.
        [[nodiscard]] Common::BoolResultStr LoadRegionFromSelection( const std::vector<Common::UUID>& selected );
        /// "Unload Selected": the selected entities' composites leave every entity list (EditorRegions::
        /// WithoutEntities); the status names how many stay held by a region, a volume or a pin.
        [[nodiscard]] Common::BoolResultStr UnloadSelected( const std::vector<Common::UUID>& selected );
        /// UE's Pin / Unpin (FLoaderAdapterPinnedActors): the selected records held whatever the other loaders
        /// are. Pin is refused by name for an entity that is not a record of the saved world.
        [[nodiscard]] Common::BoolResultStr SetSelectedPinned( const std::vector<Common::UUID>& selected,
                                                               bool                             pinned );
        [[nodiscard]] Common::BoolResultStr UnpinAll();
        /// The loader rows' names (EditorRegions::LoaderRowsOf) as the window lists them, the palette's
        /// "Load / Unload / Remove loader <name>" entries.
        [[nodiscard]] std::vector<std::string> LoaderNames() const;
        /// A row's Loaded switch ("All" off drops Load All; "Pinned" is refused - Unpin All). Refused by name for
        /// a name no row carries.
        [[nodiscard]] Common::BoolResultStr SetLoaderLoaded( const std::string& name, bool loaded );
        /// Removes a region or an entity list (a volume is removed by deleting its entity, the pins by Unpin All).
        [[nodiscard]] Common::BoolResultStr RemoveLoader( const std::string& name );
        // The default layout docks the panel (EditorLayer). A layout saved before that line has no place for
        // it and ImGui floats it: at this size the map is still readable instead of a ~30 px strip.
        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 640.0f, 720.0f };
        }

    private:
        // The Edit plan of m_Scene: a partitioned world opened from its files is planned over its descriptor
        // index (every entity of the world, loaded or not); any other scene over its serialised form.
        void RebuildEditPlan();
        // THE ONE PATH OF A LOADER CHANGE (WP-LOADER-ADAPTERS): the scene holds what @p next selects
        // (EditorRegions::LoadHeld); on success it becomes this user's state and is kept for the next open
        // (EditorLoadedRegions), on a refusal nothing changes. The undo history is reset when anything moved. Under
        // UnsavedPolicy::Ask an unsaved entity that would leave changes nothing and opens the Save / Discard / Cancel
        // prompt (DrawUnloadPrompt), whose answer is this call again (WP-OUTLINER).
        [[nodiscard]] Common::BoolResultStr Reload( ::Desert::Core::EditorRegions::HeldRegions   next,
                                                    ::Desert::Core::EditorRegions::UnsavedPolicy policy );
        // Build HLODs' second phase: build from the saved files (each landscape HLOD bound to its @p baked
        // material), write, read back.
        [[nodiscard]] Common::BoolResultStr FinishBuildHLODs( const std::vector<LandscapeHLODBaked>& baked,
                                                              const std::vector<HLODMaterialBaked>&  meshBaked );
        void DrawMap( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                      const ::Desert::Core::WorldPartitionSerialized&  partition,
                      const ::Desert::Core::Rules::ResidencyState*     residency,
                      const ::Desert::Core::WorldStreamer*             streamer );

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        ::Desert::Assets::AssetManager*        m_Assets;
        StreamerGetter                         m_Streamer;
        // The landscape BaseColor bake of a running Build HLODs (BuildHLODs -> TickBuild).
        std::unique_ptr<LandscapeHLODBake> m_Bake;
        // The merged-material bake of the mesh HLODs (WP-HLOD-MATBAKE), run after the landscape bake; what the
        // landscape bake wrote waits here until it is done.
        std::unique_ptr<HLODMaterialBake> m_MeshBake;
        std::vector<LandscapeHLODBaked>   m_LandscapeBaked;

        // Edit: the plan made from the scene, or why there is none ("not partitioned", a parse error).
        std::optional<::Desert::Core::Rules::WorldPartitionPlan> m_EditPlan;
        ::Desert::Core::WorldPartitionSerialized                 m_EditPartition;
        std::string                                              m_EditPlanStatus;
        bool                                                     m_EditPlanStale = true;
        // The scene's partition as it was when m_EditPlan was made. The plan is remade when the scene's
        // own partition stops matching it, which is how an UNDO of a conversion reaches this panel: the
        // history entry writes the scene and nothing else, so a notification would be a second path to
        // keep in step with it (and one nothing would notice going missing).
        std::optional<::Desert::Core::WorldPartitionSerialized> m_PlanSource;
        // The last conversion's refusal, shown under the button until the next attempt.
        std::string m_ConvertStatus;
        // Load Region: the rectangle selected on the map (left drag; a click selects the cell under it), the
        // drag's anchor while the button is held, and the last change's report or refusal.
        std::optional<::Desert::Core::Rules::CellBounds> m_Selection;
        std::optional<glm::dvec2>                        m_DragFrom;
        std::string                                      m_RegionStatus;
        std::string                                      m_GridEditStatus; ///< the last Runtime grids refusal
        std::string m_DataLayerStatus; ///< the last Data Layers refusal or reload
        std::array<char, 128> m_NewDataLayer{}; ///< the Data Layers section's name field
        /// What the editor holds of this world for this user - the regions, or the whole world, and the data-layer
        /// state - as EditorLoadedRegions remembers it; read once per scene, written after every reload.
        ::Desert::Core::EditorRegions::HeldRegions m_Held;
        bool                                       m_HeldRead = false;
        /// A loader change waiting on the Save / Discard / Cancel prompt: the loaders asked for (Reload; Cancel drops
        /// them, nothing having changed), the unsaved entities that would leave, and whether to open the popup.
        struct PendingUnload
        {
            ::Desert::Core::EditorRegions::HeldRegions                Next;
            std::vector<::Desert::Core::EditorRegions::UnsavedEntity> Unsaved;
            bool                                                      Open = true;
        };
        std::optional<PendingUnload>                                    m_PendingUnload;
        std::function<bool()>                                           m_SaveOpenScene;
        [[nodiscard]] const ::Desert::Core::EditorRegions::HeldRegions& Held();
        // Whether the Edit plan is the world on disk (its descriptor index) rather than the scene in memory.
        bool m_PlanFromIndex = false;
        // Edit over the index: what the editor holds of each cell of m_EditPlan (empty otherwise).
        std::vector<WorldPartitionMap::CellState> m_EditCells;
        // Edit over the index: the id of each record of m_EditPlan, and the world's Location Volumes as saved.
        std::vector<std::uint64_t>                                 m_RowIds;
        std::vector<::Desert::Core::EditorRegions::LocationVolume> m_Volumes;
        // The volumes were read once for this scene: the open applied their Auto Load, so a volume saved after it
        // joins the loaders unloaded.
        bool m_VolumesKnown = false;
        // The loader list (Loaders section), under the Load Region row.
        void DrawLoaders();
        // Every loader change starts here: the world on disk read again (RebuildEditPlan), so m_EditPlan,
        // m_RowIds, m_Volumes and Held() are its saved state. Refused, naming @p what, when the scene is not a
        // partitioned world opened from its files.
        [[nodiscard]] Common::BoolResultStr ReadWorldOnDisk( const std::string& what );
        // The loader row named @p name of what the editor holds now, or none.
        [[nodiscard]] std::optional<::Desert::Core::EditorRegions::LoaderRow>
        FindLoader( const std::string& name ) const;

        WorldPartitionMap::View m_View;
        bool                    m_FocusPending = true; // fit the plan once the canvas has a size
        bool                    m_Fitted       = false; // the view is the last fit: a resize refits it
        glm::dvec2              m_MapSize{ 0.0 };       // the canvas size the view was last drawn at
        bool                    m_Follow       = true; // Play: keep the streaming source centred (UE's default)
        // A wheel while following keeps the follow and its own scale (UE re-fits PlayerExtent = 100 m every tick
        // and drops the follow on a wheel, so a 200 m loading range with look-ahead never fits). Focus clears it.
        bool                    m_FollowZoomed = false;
        int                     m_Level        = -1;   // -1 shows every level
        // The runtime partition the map, the header and the source circles show (index into the partition's
        // RuntimePartitions, always one that makes cells):
        // UE's World Partition editor shows one runtime hash grid at a time; every grid is reachable by the
        // picker beside the Level slider.
        std::size_t             m_Grid         = 0;
        bool                    m_WasPlaying   = false;
    };
} // namespace Desert::Editor

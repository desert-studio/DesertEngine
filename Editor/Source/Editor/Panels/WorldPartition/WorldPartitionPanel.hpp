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
#include <Editor/Core/WorldPartitionHLODBuilder.hpp>
#include "WorldPartitionMap.hpp"

#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <memory>
#include <optional>
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

        /// UE's Build > Build HLODs (WorldPartitionHLODBuilder, WP-HLOD-BUILD): the HLOD rows of the world SAVED at
        /// the scene's file whose checksum changed - records, the saved meshes / materials / textures they use, the
        /// HLOD layer settings - are built and saved beside it, every other row is kept; @p options.ForceAll
        /// rebuilds every row. A scene with unsaved changes (or never saved) is not refused: this window asks to
        /// Save and build, as UE prompts to save the map before it builds HLODs from it. Refused, by name, while a
        /// build runs or for a world the build refuses. The landscape bake runs over the next editor frames.
        [[nodiscard]] Common::BoolResultStr BuildHLODs( HLODBuildOptions options = {} );

        /// The prompt's "Save and build" (and the palette's): the scene saved through the editor's one save path
        /// (SetSceneSaver), then BuildHLODs with the options the prompt was raised for.
        [[nodiscard]] Common::BoolResultStr SaveAndBuildHLODs();

        /// UE's -DeleteHLODs on this scene's world (WorldPartitionHLODBuilder.hpp DeleteHLODs). Refused while a
        /// build runs and for a scene not opened from a file.
        [[nodiscard]] Common::BoolResultStr DeleteHLODs();

        /// The editor's one save of the open scene (SceneFiles::SaveOpenScene): true when it saved.
        using SceneSaver = std::function<bool()>;
        void SetSceneSaver( SceneSaver save )
        {
            m_SaveScene = std::move( save );
        }

        /// Advances a running Build HLODs (its bake) by one editor frame; called every frame by EditorLayer,
        /// whether the panel's window is open or not.
        void TickBuild();

        /// A Build HLODs is running: its landscape bake has not finished, so its cell files are not written yet.
        [[nodiscard]] bool IsBuildingHLODs() const noexcept { return m_Builder != nullptr; }

        /// What a region change asks for: the regions to hold, or the whole world.
        struct RegionRequest
        {
            std::vector<::Desert::Core::Rules::CellBounds> Regions; // empty: the always-loaded part only
            bool                                           Whole = false;
        };
        /// UE's "Load Region from Selection" / "Unload All" / "Load All" (WP19-20, decision O3): the scene holds
        /// exactly the part of its world on disk that the regions (X/Z, cm) select - EditorRegions::LoadRegions,
        /// or the whole world - EditorRegions::LoadWholeWorld. Refused by name (a dirty entity that would leave, a
        /// scene not opened from its files). The undo history is reset on success, as UE resets its transactions
        /// when it unloads actors: an entry may name an entity that is gone.
        [[nodiscard]] Common::BoolResultStr LoadRegion( const RegionRequest& request );
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
        // The HLODs section (Edit, a world with an HLOD layer): "N stale HLOD", Build HLODs / Force all / Delete
        // HLODs, and the Save and build prompt.
        void DrawHLODBuild();
        // The saved world's HLOD status (HLODStatusOf), asked again when something it reads may have changed: the
        // plan was remade, a build or delete finished, or the content registry saw a file written (a mesh or a
        // material saved). More stale rows than before raise the notification.
        void RefreshHLODStatus();
        void DrawMap( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                      const ::Desert::Core::WorldPartitionSerialized&  partition,
                      const ::Desert::Core::Rules::ResidencyState*     residency,
                      const ::Desert::Core::WorldStreamer*             streamer );

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        ::Desert::Assets::AssetManager*        m_Assets;
        StreamerGetter                         m_Streamer;
        // The running Build HLODs (BuildHLODs -> TickBuild).
        std::unique_ptr<WorldPartitionHLODBuilder> m_Builder;
        SceneSaver                                 m_SaveScene;
        // A Build HLODs asked for on a scene with unsaved changes: the options it is to run with once saved.
        std::optional<HLODBuildOptions> m_SavePrompt;
        // The saved world's HLOD status, or why there is none, and what it was asked against.
        std::optional<HLODStatus> m_HLODStatus;
        std::string               m_HLODStatusError;
        bool                      m_HLODStatusStale = true;
        uint64_t                  m_RegistrySerial  = 0;     // ContentRegistry::WriteSerial at the last status
        std::filesystem::path     m_StatusScene;             // the scene file the status was asked for
        std::size_t               m_StaleNotified = 0;       // stale rows the last notification counted
        std::string               m_HLODBuildStatus;         // the last build / delete refusal

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
        [[nodiscard]] const ::Desert::Core::EditorRegions::HeldRegions& Held();
        // Whether the Edit plan is the world on disk (its descriptor index) rather than the scene in memory.
        bool m_PlanFromIndex = false;
        // Edit over the index: what the editor holds of each cell of m_EditPlan (empty otherwise).
        std::vector<WorldPartitionMap::CellState> m_EditCells;

        WorldPartitionMap::View m_View;
        bool                    m_FocusPending = true; // fit the plan once the canvas has a size
        bool                    m_Fitted       = false; // the view is the last fit: a resize refits it
        glm::dvec2              m_MapSize{ 0.0 };       // the canvas size the view was last drawn at
        bool                    m_Follow       = true; // Play: keep the streaming source centred (UE's default)
        // A wheel while following keeps the follow and its own scale (UE re-fits PlayerExtent = 100 m every tick
        // and drops the follow on a wheel, so a 200 m loading range with look-ahead never fits). Focus clears it.
        bool                    m_FollowZoomed = false;
        int                     m_Level        = -1;   // -1 shows every level
        // The runtime grid the map, the header and the source circles show (index into the partition's Grids):
        // UE's World Partition editor shows one runtime hash grid at a time; every grid is reachable by the
        // picker beside the Level slider.
        std::size_t             m_Grid         = 0;
        bool                    m_WasPlaying   = false;
    };
} // namespace Desert::Editor

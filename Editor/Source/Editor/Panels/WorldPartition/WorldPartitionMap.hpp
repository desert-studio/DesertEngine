#pragma once

// Ported from UE 5.8
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/SWorldPartitionEditorGrid2D.cpp :1200-1212
// (OnMouseWheel), :1190 (drag pan), :2310-2328 (FocusBox), :2333-2358 (UpdateTransform), and
// Engine/Source/Runtime/Engine/Private/LevelStreaming.cpp:2498-2513 (GetLevelStreamingStatusColor) with the tile
// opacity of WorldPartitionRuntimeSpatialHash.cpp:1055, adapted: Slate's FTransform2d becomes a translation and a
// uniform scale in double; the map plane is our ground plane (X, Z) with +X right and +Z down, so -Z (the camera's
// forward) is up; the cell state is our residency (WorldPartitionResidencyRules.hpp) instead of EStreamingStatus;
// the cells are the partition plan's (WorldPartitionRules.hpp) instead of a runtime spatial hash.
//
// THE WORLD PARTITION PANEL'S VIEW, WITHOUT IMGUI: where a world point lands in the panel, what a wheel notch and
// a drag do to that, which cells are on screen and in which order to paint them, and what colour a cell's state
// is. The panel (WorldPartitionPanel.cpp) only paints what this returns, so everything that decides the picture is
// testable (suite WorldPartitionMap).

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor::WorldPartitionMap
{
    // UE's Trans and Scale: screen = (world + Trans) * Scale + ScreenSize / 2. Trans in centimetres, Scale in
    // pixels per centimetre. UE's default scale (SWorldPartitionEditorGrid2D.cpp:477), which shows ~750 m
    // across a 1000-pixel panel.
    struct View
    {
        glm::dvec2 Trans{ 0.0 };
        double     Scale = 0.00133333332;
    };

    // UE's zoom clamp (OnMouseWheel).
    inline constexpr double kMinScale = 0.00000001;
    inline constexpr double kMaxScale = 10.0;
    // What Follow keeps on screen around the streaming source: UE's PlayerExtent, 10000 uu = 100 m.
    inline constexpr double kFollowExtentCm = 10000.0;
    // UE FocusBox leaves a 15 % margin.
    inline constexpr double kFocusFill = 0.85;

    // @p world is (X, Z) of the ground plane; the result is in panel-local pixels, origin top-left.
    [[nodiscard]] glm::dvec2 WorldToScreen( const View& view, glm::dvec2 screenSize, glm::dvec2 world );
    [[nodiscard]] glm::dvec2 ScreenToWorld( const View& view, glm::dvec2 screenSize, glm::dvec2 screen );

    // One wheel step of @p wheelDelta notches around @p mouse (panel-local): the world point under the cursor
    // stays under it.
    void Zoom( View& view, glm::dvec2 screenSize, glm::dvec2 mouse, float wheelDelta );
    // A drag of @p screenDelta pixels: the world follows the cursor.
    void Pan( View& view, glm::dvec2 screenDelta );
    // Centres @p box and fits it with UE's margin. A degenerate box keeps the scale.
    void Focus( View& view, glm::dvec2 screenSize, const ::Desert::Core::Rules::CellBounds& box );
    // UE's follow-player view in Play: the source at the centre, kFollowExtentCm on the shorter side.
    [[nodiscard]] View Follow( glm::dvec2 screenSize, glm::dvec2 sourceXZ );

    // Every reader below takes @p grid: only the cells of that runtime grid (PlannedCell::Grid, index into
    // WorldPartitionSerialized::Grids - the panel's grid picker); nullopt = every grid's cells.

    // The bounds of every cell of @p plan; nullopt for a plan without cells.
    [[nodiscard]] std::optional<::Desert::Core::Rules::CellBounds>
    PlanBounds( const ::Desert::Core::Rules::WorldPartitionPlan& plan, std::optional<std::size_t> grid = {} );

    // Cells of @p plan whose square meets the panel, in PAINT order: coarser levels first, so a finer cell is
    // drawn over the coarse one that contains it. @p level < 0 shows every level.
    [[nodiscard]] std::vector<std::size_t> VisibleCells( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                         const View& view, glm::dvec2 screenSize, int level,
                                                         std::optional<std::size_t> grid = {} );

    // The finest cell of @p plan under @p world (the one painted on top); nullopt over empty ground.
    [[nodiscard]] std::optional<std::size_t> CellAt( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                     glm::dvec2 world, int level,
                                                     std::optional<std::size_t> grid = {} );

    // The ground rectangle a drag on the map spans from @p from to @p to (world X/Z, cm), corners in any order:
    // UE's box selection on the World Partition grid (SWorldPartitionEditorGrid2D, "Load Region from Selection").
    [[nodiscard]] ::Desert::Core::Rules::CellBounds RegionBetween( glm::dvec2 from, glm::dvec2 to );

    // The finest grid level whose cell is at least @p minPixels wide on screen: the background grid's lines.
    [[nodiscard]] int GridLineLevel( const View& view, float cellSize, double minPixels );

    // "L2 · 512 m" — a level and the edge of its cells.
    [[nodiscard]] std::string LevelLabel( float cellSize, int level );

    enum class CellState
    {
        Unstreamed, // Edit: nothing streams, the cell is where the partition puts it
        Unloaded,
        Loading,
        Loaded,   // read, not yet an entity: waiting for the activation budget
        Resident, // its records are entities
        Failed,
        // Edit, a world opened by region (WP20, UE's World Partition editor colours its loaded regions): what
        // of the cell's composites the editor holds now.
        EditorLoaded,   // every one
        EditorPartial,  // some
        EditorUnloaded, // none: the cell is on disk only
    };

    // WHAT THE EDITOR HOLDS OF EACH CELL of @p plan (planned over the world's descriptor rows), one state per
    // cell, from whether each record is held - @p held( rowIds[r] ) - over the members of the cell's composites.
    // A cell with no composite is EditorUnloaded. PURE: the panel passes EntityPackages::IsLoaded.
    [[nodiscard]] std::vector<CellState> EditorCellStates( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                           std::span<const std::uint64_t>                   rowIds,
                                                           const std::function<bool( std::uint64_t )>&      held );

    // A cell's state from @p residency (the streamer's); without one, from @p editor (EditorCellStates) or
    // Unstreamed when that is empty. A unit is a cell after the plan's always-loaded composites
    // (WorldPartitionResidencyRules.hpp, UNITS).
    [[nodiscard]] CellState StateOf( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                     const ::Desert::Core::Rules::ResidencyState* residency, std::size_t cell,
                                     std::span<const CellState> editor = {} );

    // RGBA in 0..1 — UE's streaming status colour with its 2D tile opacity.
    [[nodiscard]] glm::vec4   ColorOf( CellState state );
    [[nodiscard]] const char* NameOf( CellState state );

    // UE's legend names every state a cell can be in, not only the ones on screen. In Edit that is one state
    // (three for a world opened by region: @p editor), in Play every residency (StateOf's range for a non-null
    // residency).
    [[nodiscard]] std::span<const CellState> LegendStates( bool streaming, bool editor = false );

    struct LegendRow
    {
        CellState   State;
        std::size_t Count;
    };
    // One row per LegendStates( residency != nullptr ), counting EVERY cell of @p plan at @p level (< 0: all
    // levels), on screen or not: the rows sum to the partition's cell count, so a zoomed or followed view
    // cannot make cells vanish from the legend.
    [[nodiscard]] std::vector<LegendRow> Legend( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                 const ::Desert::Core::Rules::ResidencyState* residency, int level,
                                                 std::span<const CellState> editor = {},
                                                 std::optional<std::size_t> grid   = {} );

    // The streaming source's loading circle and its unload band, in pixels.
    [[nodiscard]] double RadiusPixels( const View& view, double radiusCm );

    // ── THE HELD ENTITIES' FOOTPRINTS (UE SWorldPartitionEditorGrid2D: ShowActors, DrawActorLabels) ──
    // UE draws over the cells the bounds of every actor the editor holds once the view is close enough for a box to
    // be legible, fading it in, and its label once the label fits inside the box. Here what is drawn is a composite
    // of the plan (an entity and everything that hangs off it, placed as one): its PlannedComposite::Footprint.

    // UE's MinimumAreaCull: a footprint narrower than this on both axes is not drawn.
    inline constexpr double kMinFootprintPixels = 32.0;
    // UE's AreaFadeDistance: above the cull a footprint fades in over this many pixels.
    inline constexpr double kFootprintFadePixels = 128.0;
    // The room a label keeps from its box's edges.
    inline constexpr double kLabelPaddingPixels = 4.0;

    struct FootprintOnScreen
    {
        std::size_t Composite = 0; // index into WorldPartitionPlan::Composites
        glm::dvec2  Min{ 0.0 };    // panel-local pixels, top-left corner
        glm::dvec2  Max{ 0.0 };    // bottom-right corner
        float       Opacity = 1.0f; // UE's area fade, 0..1
    };

    // The composites of @p plan whose anchor record the editor holds - @p held( anchor record index ) - and whose
    // footprint meets the panel and spans at least kMinFootprintPixels on screen, in plan order, each with its fade.
    // A composite without a footprint (an unplaced prefab instance) has nothing to draw. PURE.
    [[nodiscard]] std::vector<FootprintOnScreen>
    VisibleFootprints( const ::Desert::Core::Rules::WorldPartitionPlan& plan, const View& view, glm::dvec2 screenSize,
                       const std::function<bool( std::size_t )>& held );

    // UE DrawActorLabel: a label of @p labelSize pixels is drawn only where it fits inside its box with
    // kLabelPaddingPixels on every side.
    [[nodiscard]] bool LabelFits( const FootprintOnScreen& footprint, glm::dvec2 labelSize );

    // ── THE HLOD OVERLAY: what Build HLODs left for each cell, judged against the saved world ──
    enum class HLODState
    {
        None,  // no HLOD built for the cell, or the cell batches nothing
        Built, // its stored HLOD draws something and was built from the cell as it is saved now
        Stale, // its stored HLOD is missing or was built from other records (WorldCells::StaleHLODCells)
    };

    // One state per cell of @p plan: Stale when @p staleUnits names its residency unit (Rules::DescribeResidencyUnit),
    // else Built when @p builtUnits does (the stored index's rows that hold a file), else None. PURE.
    [[nodiscard]] std::vector<HLODState> HLODCellStates( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                         std::span<const std::string>                     builtUnits,
                                                         std::span<const std::string>                     staleUnits );

    // The outline an HLOD state draws around its cell (None draws nothing), and its legend name.
    [[nodiscard]] glm::vec4   ColorOf( HLODState state );
    [[nodiscard]] const char* NameOf( HLODState state );
} // namespace Desert::Editor::WorldPartitionMap

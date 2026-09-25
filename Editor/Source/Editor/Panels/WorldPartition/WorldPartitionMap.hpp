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
#include <optional>
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

    // The bounds of every cell of @p plan; nullopt for a plan without cells.
    [[nodiscard]] std::optional<::Desert::Core::Rules::CellBounds>
    PlanBounds( const ::Desert::Core::Rules::WorldPartitionPlan& plan );

    // Cells of @p plan whose square meets the panel, in PAINT order: coarser levels first, so a finer cell is
    // drawn over the coarse one that contains it. @p level < 0 shows every level.
    [[nodiscard]] std::vector<std::size_t> VisibleCells( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                         const View& view, glm::dvec2 screenSize, int level );

    // The finest cell of @p plan under @p world (the one painted on top); nullopt over empty ground.
    [[nodiscard]] std::optional<std::size_t> CellAt( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                     glm::dvec2 world, int level );

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
    };

    // A cell's state from @p residency (the streamer's), or Unstreamed without one. A unit is a cell after the
    // plan's always-loaded composites (WorldPartitionResidencyRules.hpp, UNITS).
    [[nodiscard]] CellState StateOf( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                     const ::Desert::Core::Rules::ResidencyState* residency, std::size_t cell );

    // RGBA in 0..1 — UE's streaming status colour with its 2D tile opacity.
    [[nodiscard]] glm::vec4   ColorOf( CellState state );
    [[nodiscard]] const char* NameOf( CellState state );

    // The streaming source's loading circle and its unload band, in pixels.
    [[nodiscard]] double RadiusPixels( const View& view, double radiusCm );
} // namespace Desert::Editor::WorldPartitionMap

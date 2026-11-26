#pragma once

#include <Common/Core/UUID.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor::Core
{
    // UE FFoliageUISettings' tool switches, one at a time: what LMB does in the viewport.
    enum class FoliageTool : uint8_t
    {
        Paint,   ///< top up the checked types to their density under the brush
        Single,  ///< one instance per click (UE "Single")
        Select,  ///< click picks one instance; Shift adds (UE "Select")
        Lasso,   ///< the brush selects what it passes over; Shift deselects (UE "Lasso")
        Erase,   ///< thin a checked type under the brush down to Erase Density of its Density (UE "Erase")
        Remove,  ///< every instance of a checked type under the brush (UE "Remove")
        Reapply, ///< re-roll / re-check existing instances from the type's current numbers (UE "Reapply")
        Fill,    ///< click a static mesh: cover its triangles at the checked types' density (UE "Fill")
    };

    inline const char* FoliageToolName( FoliageTool tool )
    {
        switch ( tool )
        {
            case FoliageTool::Paint:
                return "Paint";
            case FoliageTool::Single:
                return "Single";
            case FoliageTool::Select:
                return "Select";
            case FoliageTool::Lasso:
                return "Lasso";
            case FoliageTool::Erase:
                return "Erase";
            case FoliageTool::Remove:
                return "Remove";
            case FoliageTool::Reapply:
                return "Reapply";
            case FoliageTool::Fill:
                return "Fill";
        }
        return "?";
    }

    // Shared state for the UE5-style Foliage paint tool (active in ViewportMode::Foliage). Multiple foliage
    // TYPES can be CHECKED for painting at once (all checked types scatter under one dab); one type is the
    // "editing" selection whose detailed settings are shown. Per-type scatter params live on
    // ECS::FoliageComponent. Brush state (radius, paint density, the tool, surface filters, the stroke seed) is
    // here.
    class FoliagePaint final
    {
    public:
        // --- types checked for painting -----------------------------------------------------------------
        static std::vector<Common::UUID>& ActiveTypes();
        static bool HasActive();
        static bool IsActive( const Common::UUID& u );
        static void ToggleActive( const Common::UUID& u );
        static void SetActiveOnly( const Common::UUID& u );
        static void ClearActive();

        // --- the type whose detailed settings are shown (last clicked row) ------------------------------
        static std::optional<Common::UUID> EditingType();
        static void SetEditingType( const Common::UUID& u );
        static void ClearEditingType();

        // --- brush --------------------------------------------------------------------------------------
        static float& BrushRadius();
        static float& PaintDensity(); // 0..1 multiplier on each type's Density
        static FoliageTool& Tool();
        static Tools::FoliageReapplySettings& Reapply();
        // UE FFoliageUISettings EraseDensity: the share of each type's Density the Erase tool leaves under the
        // brush, 0..1 (0 erases to empty, 1 leaves the type's full density).
        static float& EraseDensity();

        // --- selected instances, per foliage entity (UE FFoliageInfo::SelectedIndices) -----------------
        static std::unordered_map<Common::UUID, Tools::FoliageSelection>& Selection();
        static Tools::FoliageSelection SelectionOf( const Common::UUID& u );
        static size_t SelectedCount();
        // UE FFoliageUISettings bFilterLandscape / bFilterStaticMesh: the surfaces the brush may place on.
        static bool& FilterLandscape();
        static bool& FilterStaticMesh();

        // FO-UI1: the brush held to landscape layers (FoliageSurfaceFilter::LayerFiltered): the weight-plane
        // names the brush reads, and the weight they must reach. Empty = the filter is off.
        static std::vector<std::string>& BrushLayers();
        static float& BrushLayerMinWeight();
        static Tools::FoliageSurfaceFilter SurfaceFilter();

        // --- the palette's view (FO-UI1) ---------------------------------------------------------------
        static std::string& Search();
        static bool& GridView();
        static std::string& PresetName();
        // Where the brush would land now (the tool's hover trace), for the footprint preview; nullopt when the
        // cursor is off every surface or off the viewport.
        static std::optional<glm::vec3>& HoverPoint();
        // The view the cost column is measured from (the viewport camera, set each frame in Foliage mode).
        static glm::vec3& ViewPosition();

        // The seed of the next stroke: a stroke draws every random number from one stream seeded here, so a
        // stroke replayed with its seed places the same instances. SplitMix64 of a per-session counter, so
        // consecutive strokes do not share a stream.
        static uint64_t NextStrokeSeed();

    private:
        static std::vector<Common::UUID> s_Active;
        static std::optional<Common::UUID> s_Editing;
        static float s_Radius; // world units (cm) = 3 m
        static float s_PaintDensity;
        static FoliageTool s_Tool;
        static Tools::FoliageReapplySettings s_Reapply;
        static float s_EraseDensity;
        static std::unordered_map<Common::UUID, Tools::FoliageSelection> s_Selection;
        static bool s_FilterLandscape;
        static bool s_FilterStaticMesh;
        static uint64_t s_StrokeCounter;
        static std::vector<std::string> s_BrushLayers;
        static float s_BrushLayerMinWeight;
        static std::string s_Search;
        static bool s_GridView;
        static std::string s_PresetName;
        static std::optional<glm::vec3> s_Hover;
        static glm::vec3 s_View;
    };
} // namespace Desert::Editor::Core

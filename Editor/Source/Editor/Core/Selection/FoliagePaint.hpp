#pragma once

#include <Common/Core/UUID.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
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
        static std::vector<Common::UUID>& ActiveTypes() { return s_Active; }
        static bool                        HasActive() { return !s_Active.empty(); }
        static bool IsActive( const Common::UUID& u )
        {
            return std::find( s_Active.begin(), s_Active.end(), u ) != s_Active.end();
        }
        static void ToggleActive( const Common::UUID& u )
        {
            const auto it = std::find( s_Active.begin(), s_Active.end(), u );
            if ( it != s_Active.end() )
                s_Active.erase( it );
            else
                s_Active.push_back( u );
        }
        static void SetActiveOnly( const Common::UUID& u )
        {
            s_Active.clear();
            s_Active.push_back( u );
        }
        static void ClearActive() { s_Active.clear(); }

        // --- the type whose detailed settings are shown (last clicked row) ------------------------------
        static std::optional<Common::UUID> EditingType() { return s_Editing; }
        static void                        SetEditingType( const Common::UUID& u ) { s_Editing = u; }
        static void                        ClearEditingType() { s_Editing.reset(); }

        // --- brush --------------------------------------------------------------------------------------
        static float& BrushRadius() { return s_Radius; }
        static float& PaintDensity() { return s_PaintDensity; } // 0..1 multiplier on each type's Density
        static FoliageTool& Tool()
        {
            return s_Tool;
        }
        static Tools::FoliageReapplySettings& Reapply()
        {
            return s_Reapply;
        }
        // The panel's "Move by" offset for the selected instances, cm.
        static glm::vec3& MoveOffset()
        {
            return s_MoveOffset;
        }

        // --- selected instances, per foliage entity (UE FFoliageInfo::SelectedIndices) -----------------
        static std::unordered_map<Common::UUID, Tools::FoliageSelection>& Selection()
        {
            return s_Selection;
        }
        static Tools::FoliageSelection SelectionOf( const Common::UUID& u )
        {
            const auto it = s_Selection.find( u );
            return it == s_Selection.end() ? Tools::FoliageSelection{} : it->second;
        }
        static size_t SelectedCount()
        {
            size_t n = 0;
            for ( const auto& [id, selected] : s_Selection )
                n += selected.size();
            return n;
        }
        // UE FFoliageUISettings bFilterLandscape / bFilterStaticMesh: the surfaces the brush may place on.
        static bool& FilterLandscape()
        {
            return s_FilterLandscape;
        }
        static bool& FilterStaticMesh()
        {
            return s_FilterStaticMesh;
        }

        // The seed of the next stroke: a stroke draws every random number from one stream seeded here, so a
        // stroke replayed with its seed places the same instances. SplitMix64 of a per-session counter, so
        // consecutive strokes do not share a stream.
        static uint64_t NextStrokeSeed()
        {
            uint64_t z = ( s_StrokeCounter += 0x9E3779B97F4A7C15ull );
            z          = ( z ^ ( z >> 30u ) ) * 0xBF58476D1CE4E5B9ull;
            z          = ( z ^ ( z >> 27u ) ) * 0x94D049BB133111EBull;
            return z ^ ( z >> 31u );
        }

    private:
        static inline std::vector<Common::UUID>   s_Active;
        static inline std::optional<Common::UUID> s_Editing;
        static inline float                       s_Radius       = 300.0f; // world units (cm) = 3 m
        static inline float                       s_PaintDensity = 1.0f;
        static inline FoliageTool                   s_Tool         = FoliageTool::Paint;
        static inline Tools::FoliageReapplySettings s_Reapply;
        static inline glm::vec3                     s_MoveOffset = glm::vec3( 0.0f, 100.0f, 0.0f );
        static inline std::unordered_map<Common::UUID, Tools::FoliageSelection> s_Selection;
        static inline bool                                                      s_FilterLandscape  = true;
        static inline bool                                                      s_FilterStaticMesh = true;
        static inline uint64_t                                                  s_StrokeCounter    = 0u;
    };
} // namespace Desert::Editor::Core

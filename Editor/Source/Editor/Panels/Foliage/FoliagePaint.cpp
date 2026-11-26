#include "FoliagePaint.hpp"

namespace Desert::Editor::Core
{
    auto FoliagePaint::NextStrokeSeed() -> uint64_t
    {
        uint64_t z = ( s_StrokeCounter += 0x9E3779B97F4A7C15ull );
        z          = ( z ^ ( z >> 30u ) ) * 0xBF58476D1CE4E5B9ull;
        z          = ( z ^ ( z >> 27u ) ) * 0x94D049BB133111EBull;
        return z ^ ( z >> 31u );
    }

    auto FoliagePaint::ViewPosition() -> glm::vec3&
    {
        return s_View;
    }

    auto FoliagePaint::HoverPoint() -> std::optional<glm::vec3>&
    {
        return s_Hover;
    }

    auto FoliagePaint::PresetName() -> std::string&
    {
        return s_PresetName;
    }

    auto FoliagePaint::GridView() -> bool&
    {
        return s_GridView;
    }

    auto FoliagePaint::Search() -> std::string&
    {
        return s_Search;
    }

    auto FoliagePaint::SurfaceFilter() -> Tools::FoliageSurfaceFilter
    {
        Tools::FoliageSurfaceFilter filter;
        filter.Landscape          = s_FilterLandscape;
        filter.StaticMesh         = s_FilterStaticMesh;
        filter.LayerFiltered      = !s_BrushLayers.empty();
        filter.MinimumLayerWeight = s_BrushLayerMinWeight;
        return filter;
    }

    auto FoliagePaint::BrushLayerMinWeight() -> float&
    {
        return s_BrushLayerMinWeight;
    }

    auto FoliagePaint::BrushLayers() -> std::vector<std::string>&
    {
        return s_BrushLayers;
    }

    auto FoliagePaint::FilterStaticMesh() -> bool&
    {
        return s_FilterStaticMesh;
    }

    auto FoliagePaint::FilterLandscape() -> bool&
    {
        return s_FilterLandscape;
    }

    auto FoliagePaint::SelectedCount() -> size_t
    {
        size_t n = 0;
        for ( const auto& [id, selected] : s_Selection )
            n += selected.size();
        return n;
    }

    auto FoliagePaint::SelectionOf( const Common::UUID& u ) -> Tools::FoliageSelection
    {
        const auto it = s_Selection.find( u );
        return it == s_Selection.end() ? Tools::FoliageSelection{} : it->second;
    }

    auto FoliagePaint::Selection() -> std::unordered_map<Common::UUID, Tools::FoliageSelection>&
    {
        return s_Selection;
    }

    auto FoliagePaint::EraseDensity() -> float&
    {
        return s_EraseDensity;
    }

    auto FoliagePaint::Reapply() -> Tools::FoliageReapplySettings&
    {
        return s_Reapply;
    }

    auto FoliagePaint::Tool() -> FoliageTool&
    {
        return s_Tool;
    }

    auto FoliagePaint::PaintDensity() -> float&
    {
        return s_PaintDensity;
    }

    auto FoliagePaint::BrushRadius() -> float&
    {
        return s_Radius;
    }

    auto FoliagePaint::ClearEditingType() -> void
    {
        s_Editing.reset();
    }

    auto FoliagePaint::SetEditingType( const Common::UUID& u ) -> void
    {
        s_Editing = u;
    }

    auto FoliagePaint::EditingType() -> std::optional<Common::UUID>
    {
        return s_Editing;
    }

    auto FoliagePaint::ClearActive() -> void
    {
        s_Active.clear();
    }

    auto FoliagePaint::SetActiveOnly( const Common::UUID& u ) -> void
    {
        s_Active.clear();
        s_Active.push_back( u );
    }

    auto FoliagePaint::ToggleActive( const Common::UUID& u ) -> void
    {
        const auto it = std::find( s_Active.begin(), s_Active.end(), u );
        if ( it != s_Active.end() )
            s_Active.erase( it );
        else
            s_Active.push_back( u );
    }

    auto FoliagePaint::IsActive( const Common::UUID& u ) -> bool
    {
        return std::find( s_Active.begin(), s_Active.end(), u ) != s_Active.end();
    }

    auto FoliagePaint::HasActive() -> bool
    {
        return !s_Active.empty();
    }

    auto FoliagePaint::ActiveTypes() -> std::vector<Common::UUID>&
    {
        return s_Active;
    }

    std::vector<Common::UUID> FoliagePaint::s_Active;

    std::optional<Common::UUID> FoliagePaint::s_Editing;

    float FoliagePaint::s_Radius = 300.0f;

    float FoliagePaint::s_PaintDensity = 1.0f;

    FoliageTool FoliagePaint::s_Tool = FoliageTool::Paint;

    Tools::FoliageReapplySettings FoliagePaint::s_Reapply;

    float FoliagePaint::s_EraseDensity = 0.0f;

    std::unordered_map<Common::UUID, Tools::FoliageSelection> FoliagePaint::s_Selection;

    bool FoliagePaint::s_FilterLandscape = true;

    bool FoliagePaint::s_FilterStaticMesh = true;

    uint64_t FoliagePaint::s_StrokeCounter = 0u;

    std::vector<std::string> FoliagePaint::s_BrushLayers;

    float FoliagePaint::s_BrushLayerMinWeight = 0.5f;

    std::string FoliagePaint::s_Search;

    bool FoliagePaint::s_GridView = false;

    std::string FoliagePaint::s_PresetName = "Palette";

    std::optional<glm::vec3> FoliagePaint::s_Hover;

    glm::vec3 FoliagePaint::s_View = glm::vec3( 0.0f );
}

#include "FracturePanel.hpp"

#include "FractureCommands.hpp"
#include "FractureTool.hpp"

#include <Editor/Core/AssetPickerRows.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>

#include <Engine/Assets/ContentRegistry.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace Desert::Editor
{
    FracturePanel::FracturePanel( const std::shared_ptr<Desert::Core::Scene>& scene )
         : IPanel( "Fracture", /*showPanel=*/false ) // contextual: the mode opens it
           ,
           m_Scene( scene )
    {
    }

    bool FracturePanel::IsRelevant() const
    {
        return Core::ViewportMode::Get() == Core::EditorMode::Fracture;
    }

    void FracturePanel::OnUIRender()
    {
        // The panel draws the file, not a stale copy: an undo / redo of a Generate shows on this frame.
        FractureTool& tool = FractureTool::Get();
        tool.Refresh();
        DrawTarget();
        ImGui::Separator();
        DrawGenerate();
        ImGui::Separator();
        DrawInteriorMaterial();
        ImGui::Separator();
        DrawView();
        if ( !tool.Status().empty() )
            ImGui::TextWrapped( "%s", tool.Status().c_str() );
    }

    void FracturePanel::DrawTarget()
    {
        FractureTool& tool = FractureTool::Get();
        ImGui::TextUnformatted( "Geometry Collection (.dfrac, under Assets)" );
        std::array<char, 512> path{};
        std::copy_n( tool.Path.begin(), std::min( tool.Path.size(), path.size() - 1 ), path.begin() );
        if ( ImGui::InputText( "##FracturePath", path.data(), path.size() ) )
            tool.Path = path.data();
        ImGui::SameLine();
        if ( ImGui::Button( "Load" ) )
            tool.Load();
        if ( tool.Loaded() )
            ImGui::Text( "%zu nodes, %u levels", tool.Fracture().Nodes.size(),
                         Destruction::DeepestLevel( tool.Fracture().Nodes ) );
    }

    void FracturePanel::DrawGenerate()
    {
        FractureTool& tool = FractureTool::Get();
        ImGui::TextUnformatted( "Generate" );
        int seed = static_cast<int>( tool.Settings.Seed & 0x7fffffff );
        if ( ImGui::InputInt( "Random Seed", &seed ) )
            tool.Settings.Seed = static_cast<uint64_t>( std::max( seed, 0 ) );

        // UE fractures the selected level again to make the next one; ours keeps the list and re-bakes it all,
        // so every level is reproducible from the file's settings.
        auto& levels = tool.Settings.Levels;
        for ( size_t i = 0; i < levels.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            DrawLevel( i, levels[i] );
            ImGui::PopID();
        }
        if ( ImGui::Button( "Add Level" ) )
            levels.push_back( levels.empty() ? Destruction::FractureLevelSettings{} : levels.back() );
        ImGui::SameLine();
        ImGui::BeginDisabled( levels.size() <= 1 );
        if ( ImGui::Button( "Remove Level" ) )
            levels.pop_back();
        ImGui::EndDisabled();

        ImGui::Checkbox( "Auto Cluster (grid)", &tool.Settings.AutoCluster.Enabled );
        if ( tool.Settings.AutoCluster.Enabled )
            ImGui::InputInt3( "Grid", &tool.Settings.AutoCluster.GridX );

        // A refusal is shown as the tool's Status() below.
        if ( ImGui::Button( "Fracture (Generate)" ) )
            static_cast<void>( GenerateFromSelection( tool, m_Scene.lock() ) );
    }

    void FracturePanel::DrawLevel( size_t index, Destruction::FractureLevelSettings& level )
    {
        ImGui::Text( "Level %zu", index + 1 );
        int method = static_cast<int>( level.Method );
        if ( ImGui::Combo( "Method", &method, kFractureMethodNames.data(),
                           static_cast<int>( kFractureMethodNames.size() ) ) )
            level.Method = static_cast<Destruction::FractureMethod>( method );
        switch ( level.Method )
        {
            case Destruction::FractureMethod::Uniform:
                ImGui::InputInt( "Sites", &level.SiteCount );
                break;
            case Destruction::FractureMethod::Clustered:
                ImGui::InputInt( "Clusters", &level.Clusters );
                ImGui::InputInt( "Sites Per Cluster", &level.SitesPerCluster );
                ImGui::InputDouble( "Min Radius (cm)", &level.MinRadius );
                ImGui::InputDouble( "Max Radius (cm)", &level.MaxRadius );
                break;
            case Destruction::FractureMethod::Planar:
                DrawPlanes( level );
                break;
            case Destruction::FractureMethod::Brick:
                ImGui::InputDouble( "Brick Length (cm)", &level.Brick.Length );
                ImGui::InputDouble( "Brick Height (cm)", &level.Brick.Height );
                ImGui::InputDouble( "Brick Depth (cm)", &level.Brick.Depth );
                break;
        }
        ImGui::InputFloat( "Damage Threshold", &level.DamageThreshold );
    }

    void FracturePanel::DrawPlanes( Destruction::FractureLevelSettings& level )
    {
        // UE's Planar tool places its cutting planes with a gizmo; ours are typed: a normal (any length) and a
        // point the plane passes through, in the source mesh's space (cm) - exactly what FractureBake cuts with.
        for ( size_t p = 0; p < level.Planes.size(); )
        {
            ImGui::PushID( static_cast<int>( p ) );
            Destruction::CutPlane& plane = level.Planes[p];
            ImGui::InputScalarN( "Normal", ImGuiDataType_Double, &plane.Normal.x, 3 );
            ImGui::InputScalarN( "Point (cm)", ImGuiDataType_Double, &plane.Point.x, 3 );
            const bool remove = ImGui::Button( "Remove Plane" );
            ImGui::PopID();
            if ( remove )
                level.Planes.erase( level.Planes.begin() + static_cast<std::ptrdiff_t>( p ) );
            else
                ++p;
        }
        if ( ImGui::Button( "Add Plane" ) )
            level.Planes.push_back( Destruction::CutPlane{} ); // across Z, through the origin
    }

    void FracturePanel::DrawInteriorMaterial()
    {
        // UE's Internal Material, picked from the project's materials the way the editor's other material
        // pickers list them (ContentRegistry rows, PickerDisplayName); the fracture stores the asset's GUID.
        FractureTool&                    tool    = FractureTool::Get();
        const Common::Content::AssetGuid current = tool.Fracture().InteriorMaterial;
        const auto  rows    = Assets::ContentRegistry::Rows( Common::Content::ContentKind::Material );
        std::string preview = current.IsNull() ? std::string( "None" ) : std::string( "(missing material)" );
        for ( const auto& row : rows )
            if ( !current.IsNull() && row.Guid.has_value() && *row.Guid == current )
                preview = PickerDisplayName( row );
        ImGui::BeginDisabled( !tool.Loaded() );
        if ( ImGui::BeginCombo( "Internal Material", preview.c_str() ) )
        {
            // A refusal is shown as the tool's Status().
            if ( ImGui::Selectable( "None", current.IsNull() ) && !current.IsNull() )
                static_cast<void>( tool.SetInteriorMaterial( {} ) );
            for ( const auto& row : rows )
            {
                if ( !row.Guid.has_value() )
                    continue; // a material file that states no GUID cannot be referenced by one
                const bool chosen = *row.Guid == current;
                if ( ImGui::Selectable( ( PickerDisplayName( row ) + "##" + row.Key ).c_str(), chosen ) &&
                     !chosen )
                    static_cast<void>( tool.SetInteriorMaterial( *row.Guid ) );
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    void FracturePanel::DrawView()
    {
        FractureTool&                    tool     = FractureTool::Get();
        const Destruction::FractureData& fracture = tool.Fracture();
        ImGui::TextUnformatted( "View (preview only, not saved)" );
        ImGui::SliderFloat( "Explode Amount", &tool.View.ExplodeAmount, 0.0f, 1.0f );
        const int deepest = static_cast<int>( Destruction::DeepestLevel( fracture.Nodes ) );
        ImGui::SliderInt( "Fracture Level (-1 = all)", &tool.View.ViewLevel, -1, std::max( deepest, 0 ) );
        if ( !tool.Loaded() )
            return;

        // The preview: every leaf's hull from above (X right, Z down the canvas), moved by its exploded offset and
        // tinted by level (UE's bone colours).
        const auto   offsets = Destruction::ExplodedOffsets( fracture.Nodes, tool.View );
        const ImVec2 origin  = ImGui::GetCursorScreenPos();
        const float  side    = std::max( 160.0f, ImGui::GetContentRegionAvail().x );
        ImGui::InvisibleButton( "##FracturePreview", ImVec2( side, side ) );
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRect( origin, ImVec2( origin.x + side, origin.y + side ), IM_COL32( 90, 90, 90, 255 ) );

        double reach = 1.0;
        for ( size_t i = 0; i < fracture.Nodes.size(); ++i )
            for ( const glm::vec3& v : fracture.Nodes[i].HullVertices )
                reach = std::max( { reach, std::abs( v.x + offsets[i].x ), std::abs( v.z + offsets[i].z ) } );
        const float  scale = static_cast<float>( 0.45 * side / reach );
        const ImVec2 centre( origin.x + 0.5f * side, origin.y + 0.5f * side );

        constexpr std::array<ImU32, 4> kLevelColours = {
             IM_COL32( 230, 120, 60, 255 ), IM_COL32( 80, 170, 230, 255 ), IM_COL32( 120, 210, 90, 255 ),
             IM_COL32( 220, 200, 70, 255 ) };
        for ( size_t i = 0; i < fracture.Nodes.size(); ++i )
        {
            const Destruction::FractureNode& node   = fracture.Nodes[i];
            const ImU32                      colour = kLevelColours[node.Level % kLevelColours.size()];
            for ( const std::vector<int>& face : node.HullFaces )
                for ( size_t k = 0; k < face.size(); ++k )
                {
                    const glm::vec3& a = node.HullVertices[static_cast<size_t>( face[k] )];
                    const glm::vec3& b = node.HullVertices[static_cast<size_t>( face[( k + 1 ) % face.size()] )];
                    draw->AddLine( ImVec2( centre.x + scale * static_cast<float>( a.x + offsets[i].x ),
                                           centre.y + scale * static_cast<float>( a.z + offsets[i].z ) ),
                                   ImVec2( centre.x + scale * static_cast<float>( b.x + offsets[i].x ),
                                           centre.y + scale * static_cast<float>( b.z + offsets[i].z ) ),
                                   colour );
                }
        }
    }
} // namespace Desert::Editor

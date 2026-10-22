#include "MeshLODRows.hpp"

#include <Common/Core/Logger.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <ImGui/imgui.h>

#include <format>
#include <string>
#include <vector>

namespace Desert::Editor::MeshLODRows
{
    std::size_t LevelCount( const ::Desert::Mesh& mesh )
    {
        std::size_t levels = 0;
        for ( const auto& sm : mesh.GetSubmeshes() )
            levels = sm.LODs.size() > levels ? sm.LODs.size() : levels;
        return levels;
    }

    std::uint32_t Triangles( const ::Desert::Mesh& mesh, const std::size_t level )
    {
        std::uint32_t tris = 0;
        for ( const auto& sm : mesh.GetSubmeshes() )
        {
            if ( sm.LODs.empty() )
                continue;
            const std::size_t li = level < sm.LODs.size() ? level : sm.LODs.size() - 1;
            tris += sm.LODs[li].IndexCount / 3;
        }
        return tris;
    }

    std::optional<std::size_t> DrawingLevel( const Core::Scene* scene, const ECS::Entity& entity,
                                             const ::Desert::Mesh& mesh, const int forcedLOD, const int lodBias )
    {
        const std::size_t levels = LevelCount( mesh );
        if ( !scene || levels == 0 )
            return std::nullopt;
        const auto& camera = scene->GetActiveCamera();
        if ( !camera )
            return std::nullopt;
        const std::size_t level = Geometry::SelectLOD(
             entity.GetWorldTransform(), mesh.GetSubmeshes(),
             Geometry::LODView{ camera->GetPosition(), camera->GetProjectionMatrix() }, mesh.GetLODScreenSizes(),
             forcedLOD, lodBias );
        return level < levels ? level : levels - 1;
    }

    void DrawLevels( const ::Desert::Mesh& mesh, const std::optional<std::size_t> marked, const char* markLabel )
    {
        const std::size_t        levels = LevelCount( mesh );
        const std::vector<float> sizes  = mesh.GetLODScreenSizes();
        if ( !ImGui::BeginTable( "##lodlevels", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
            return;
        ImGui::TableSetupColumn( "LOD" );
        ImGui::TableSetupColumn( "Triangles" );
        ImGui::TableSetupColumn( "Screen Size" );
        ImGui::TableSetupColumn( "" );
        ImGui::TableHeadersRow();
        for ( std::size_t l = 0; l < levels; ++l )
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text( "LOD %zu", l );
            ImGui::TableNextColumn();
            ImGui::Text( "%u", Triangles( mesh, l ) );
            ImGui::TableNextColumn();
            if ( l < sizes.size() )
                ImGui::Text( "%.4f", sizes[l] );
            else
                ImGui::TextDisabled( "-" );
            ImGui::TableNextColumn();
            if ( marked && *marked == l )
                ImGui::TextUnformatted( markLabel );
        }
        ImGui::EndTable();
    }

    void DrawScreenSizes( const Assets::AssetHandle& meshHandle, ::Desert::Mesh& mesh )
    {
        namespace Ser                  = Assets::Serialization;
        const Assets::MeshAsset* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( meshHandle );
        const auto home = asset ? Ser::LodScreenSizesHomeOf( asset->GetMetadata().Filepath ) : std::nullopt;

        const auto write = [&]( const std::optional<std::vector<float>>& sizes )
        {
            if ( !home )
                return;
            if ( const auto written =
                      Ser::SetImportRecordLodScreenSizes( home->Source, home->MeshFile, sizes );
                 !written )
                LOG_ERROR( "[Details] LOD screen sizes of '{}': {}", home->MeshFile, written.GetError() );
        };

        Utils::ImGuiUtilities::ResetPropertyRows();
        ImGui::BeginDisabled( !home.has_value() );

        bool autoCompute = !mesh.GetAuthoredLODScreenSizes().has_value();
        Utils::ImGuiUtilities::BeginPropertyRow(
             "Auto Compute Screen Size",
             home ? "Each level's screen size computed by the engine; untick to set them per level"
                  : "This mesh was written by no import, so its screen sizes are computed and have no home" );
        if ( ImGui::Checkbox( "##lodautoscreen", &autoCompute ) )
        {
            // Unticked: the computed sizes become the authored ones (UE copies the render data's sizes into
            // the source models when bAutoComputeLODScreenSize is turned off).
            std::optional<std::vector<float>> next;
            if ( !autoCompute )
                next = mesh.GetLODScreenSizes();
            mesh.SetAuthoredLODScreenSizes( next );
            write( next );
        }
        Utils::ImGuiUtilities::EndPropertyRow();

        const std::vector<float> sizes = mesh.GetLODScreenSizes();
        ImGui::BeginDisabled( autoCompute );
        for ( std::size_t level = 1; level < sizes.size(); ++level )
        {
            // Kept between its neighbours, so a coarser level never begins at a larger size than a finer one.
            const float       upper = sizes[level - 1];
            const float       lower = level + 1 < sizes.size() ? sizes[level + 1] : 0.0001f;
            float             value = sizes[level];
            const std::string label = std::format( "LOD {} Screen Size", level );
            Utils::ImGuiUtilities::BeginPropertyRow(
                 label.c_str(),
                 "The bounding sphere's diameter, as a fraction of the screen, below which this "
                 "level is drawn" );
            ImGui::PushID( static_cast<int>( level ) );
            if ( ImGui::DragFloat( "##lodscreensize", &value, 0.002f, lower, upper, "%.4f",
                                   ImGuiSliderFlags_AlwaysClamp ) )
            {
                std::vector<float> next = sizes;
                next[level]             = value;
                mesh.SetAuthoredLODScreenSizes( next );
            }
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                write( mesh.GetAuthoredLODScreenSizes() );
            ImGui::PopID();
            Utils::ImGuiUtilities::EndPropertyRow();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
} // namespace Desert::Editor::MeshLODRows

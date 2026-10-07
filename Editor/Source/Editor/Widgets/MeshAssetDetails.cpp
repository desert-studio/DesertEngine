#include "MeshAssetDetails.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/NumberFormat.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Import/ImportOptionsDialog.hpp>

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Geometry/MeshStats.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <format>
#include <string>

namespace Desert::Editor::MeshAssetDetails
{
    namespace
    {
        // UE's Material Slots list is the model: one line per element, identified by index and name, with its
        // own weight beside it. The index leads — it is what the material slot mapping uses; the imported name
        // is a hint.
        void DrawElements( const Assets::MeshAsset& asset )
        {
            const auto& sections = asset.GetSubmeshes();
            const auto  headline = std::format( "{} elements", sections.size() );
            if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_HEXAGON_MULTIPLE "  Elements", true,
                                                        headline.c_str() ) )
                return;

            const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
            if ( !ImGui::BeginTable( "##mesh_elements", 4, flags ) )
                return;
            ImGui::TableSetupColumn( "Element", ImGuiTableColumnFlags_WidthStretch, 0.40f );
            ImGui::TableSetupColumn( "Tris", ImGuiTableColumnFlags_WidthStretch, 0.22f );
            ImGui::TableSetupColumn( "Verts", ImGuiTableColumnFlags_WidthStretch, 0.22f );
            ImGui::TableSetupColumn( "LODs", ImGuiTableColumnFlags_WidthStretch, 0.16f );
            ImGui::TableHeadersRow();

            for ( std::size_t index = 0; index < sections.size(); ++index )
            {
                const auto& section = sections[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string name = section.Name.empty()
                                              ? std::format( "Element {}", index )
                                              : std::format( "Element {}  {}", index, section.Name );
                ImGui::TextUnformatted( name.c_str() );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( FormatThousands( Geometry::SubmeshTriangles( section ) ).c_str() );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( FormatThousands( section.VertexCount ).c_str() );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                     FormatThousands( std::max<std::size_t>( section.LODs.size(), 1 ) ).c_str() );
            }
            ImGui::EndTable();
        }
    } // namespace

    void Draw( const Assets::MeshAsset& asset )
    {
        DrawElements( asset );
        // UE's Import Settings category, under the asset's facts: the source's options and Reimport (THM1l).
        ImportOptions::DrawImportSettingsSection( asset.GetMetadata().Filepath );
    }

    SkinningAudit AuditSkinning( const std::span<const SkinnedVertex> vertices, const std::size_t boneCount )
    {
        SkinningAudit audit;
        for ( const auto& v : vertices )
        {
            float       weight   = 0.0f;
            std::size_t active   = 0;
            bool        outRange = false;
            for ( std::size_t i = 0; i < SkinnedVertex::MAX_BONE_INFLUENCES; ++i )
            {
                if ( v.BoneWeights[i] <= 0.0f )
                    continue;
                weight += v.BoneWeights[i];
                ++active;
                if ( v.BoneIDs[i] >= boneCount )
                    outRange = true;
            }
            if ( weight <= 0.0f )
                ++audit.Unweighted;
            if ( outRange )
                ++audit.OutOfRange;
            if ( active == SkinnedVertex::MAX_BONE_INFLUENCES )
                ++audit.FullyInfluenced;
        }
        return audit;
    }

    void DrawSkinningAudit( const SkinningAudit& audit )
    {
        // Wrapped, because a warning that runs off the edge of a docked panel is a warning nobody reads.
        ImGui::PushTextWrapPos( 0.0f );
        if ( audit.OutOfRange > 0 )
            ImGui::TextColored( ThemeManager::GetErrorColor(),
                                ICON_MDI_ALERT " %llu vertices reference a bone this skeleton does not have - the "
                                               "mesh is bound to the wrong rig",
                                static_cast<unsigned long long>( audit.OutOfRange ) );
        if ( audit.Unweighted > 0 )
            ImGui::TextColored( ThemeManager::GetWarningColor(),
                                ICON_MDI_ALERT
                                " %llu vertices have no bone weights - they stay in bind pose while "
                                "the rest animates",
                                static_cast<unsigned long long>( audit.Unweighted ) );
        ImGui::PopTextWrapPos();
        if ( audit.FullyInfluenced > 0 )
        {
            ImGui::TextDisabled( "%llu vertices use all %zu influence slots",
                                 static_cast<unsigned long long>( audit.FullyInfluenced ),
                                 SkinnedVertex::MAX_BONE_INFLUENCES );
            Utils::ImGuiUtilities::Tooltip(
                 "The import keeps the 4 heaviest influences per vertex; weights beyond "
                 "that were dropped." );
        }
    }
} // namespace Desert::Editor::MeshAssetDetails

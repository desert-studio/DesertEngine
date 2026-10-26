#include "Controls.hpp"

#include <ImGui/imgui_internal.h>

#include <cmath>

namespace Desert::Editor
{

    namespace ImGui = ::ImGui;

    void Widgets::DrawVec3Control( const std::string& label, glm::vec3& values, float resetValue )
    {
        ImGui::PushID( label.c_str() );

        ImGui::Columns( 2 );
        ImGui::SetColumnWidth( 0, 100.0f );

        ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.9f, 0.9f, 0.9f, 0.9f ) );
        ImGui::TextUnformatted( label.c_str() );
        ImGui::PopStyleColor();

        ImGui::NextColumn();

        ImGui::PushMultiItemsWidths( 3, ImGui::CalcItemWidth() );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2{ 4, 2 } );

        float  lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
        ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

        {
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4{ 0.65f, 0.15f, 0.15f, 1.0f } );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4{ 0.75f, 0.2f, 0.2f, 1.0f } );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4{ 0.55f, 0.1f, 0.1f, 1.0f } );
            if ( ImGui::Button( "X", buttonSize ) )
                values.x = resetValue;
            ImGui::PopStyleColor( 3 );

            ImGui::SameLine();
            ImGui::PushStyleColor( ImGuiCol_FrameBg, ImVec4( 0.11f, 0.11f, 0.12f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 0.13f, 0.13f, 0.14f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 0.09f, 0.09f, 0.10f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.95f, 0.95f, 0.95f, 1.0f ) );
            ImGui::DragFloat( "##X", &values.x, 0.1f );
            ImGui::PopStyleColor( 4 );
            ImGui::PopItemWidth();
            ImGui::SameLine();
        }

        {
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4{ 0.15f, 0.55f, 0.15f, 1.0f } );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4{ 0.2f, 0.65f, 0.2f, 1.0f } );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4{ 0.1f, 0.45f, 0.1f, 1.0f } );
            if ( ImGui::Button( "Y", buttonSize ) )
                values.y = resetValue;
            ImGui::PopStyleColor( 3 );

            ImGui::SameLine();
            ImGui::PushStyleColor( ImGuiCol_FrameBg, ImVec4( 0.11f, 0.11f, 0.12f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 0.13f, 0.13f, 0.14f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 0.09f, 0.09f, 0.10f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.95f, 0.95f, 0.95f, 1.0f ) );
            ImGui::DragFloat( "##Y", &values.y, 0.1f );
            ImGui::PopStyleColor( 4 );
            ImGui::PopItemWidth();
            ImGui::SameLine();
        }

        {
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4{ 0.15f, 0.15f, 0.65f, 1.0f } );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4{ 0.2f, 0.2f, 0.75f, 1.0f } );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4{ 0.1f, 0.1f, 0.55f, 1.0f } );
            if ( ImGui::Button( "Z", buttonSize ) )
                values.z = resetValue;
            ImGui::PopStyleColor( 3 );

            ImGui::SameLine();
            ImGui::PushStyleColor( ImGuiCol_FrameBg, ImVec4( 0.11f, 0.11f, 0.12f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 0.13f, 0.13f, 0.14f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 0.09f, 0.09f, 0.10f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.95f, 0.95f, 0.95f, 1.0f ) );
            ImGui::DragFloat( "##Z", &values.z, 0.1f );
            ImGui::PopStyleColor( 4 );
            ImGui::PopItemWidth();
        }

        ImGui::PopStyleVar();
        ImGui::Columns( 1 );
        ImGui::PopID();
    }

} // namespace Desert::Editor
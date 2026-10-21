#include "Editor/Panels/Debug/ParticleDebuggerPanel.hpp"

#include <ImGui/imgui.h>
#include <Engine/Desert.hpp>
#include <Engine/Graphic/Systems/Scene/Particles/ParticleWorldGpu.hpp>

#include <string>

namespace Desert::Editor
{
    using Graphic::System::kParticleDebugMaxRows;
    using Graphic::System::ParticleDebugRequest;
    using Graphic::System::ParticleEmitterDebug;
    using Graphic::System::ParticleWorldGpu;

    ParticleDebuggerPanel::ParticleDebuggerPanel( const std::shared_ptr<Core::Scene>& scene )
         : IPanel( "Particle Debugger", false ), m_Scene( scene )
    {
    }

    void ParticleDebuggerPanel::SetScene( const std::shared_ptr<Core::Scene>& scene )
    {
        SendRequest( false ); // the previous scene stops reading back
        m_Scene    = scene;
        m_Selected = false;
        m_FirstRow = 0;
    }

    void ParticleDebuggerPanel::SendRequest( const bool enabled ) const
    {
        const std::shared_ptr<Core::Scene> scene = m_Scene.lock();
        if ( !scene )
            return;
        ParticleDebugRequest request;
        request.Enabled  = enabled;
        request.EntityId = m_Selected ? m_EntityId : 0xFFFFFFFFu; // no table without a selection
        request.Emitter  = m_Emitter;
        request.FirstRow = m_FirstRow;
        ParticleWorldGpu::Of( scene->GetVFXWorld() ).SetDebugRequest( request );
    }

    void ParticleDebuggerPanel::OnUIRender()
    {
        bool& open = GetVisibility();
        SendRequest( open );
        if ( !open )
            return;
        if ( !ImGui::Begin( GetName().c_str(), &open ) )
        {
            ImGui::End();
            return;
        }
        const std::shared_ptr<Core::Scene> scene = m_Scene.lock();
        if ( !scene )
        {
            ImGui::TextUnformatted( "No scene." );
            ImGui::End();
            return;
        }
        const auto& emitters = ParticleWorldGpu::Of( scene->GetVFXWorld() ).DebugEmitters();
        ImGui::Text( "%zu emitter(s) - GPU counts of the last finished frame (no stall)", emitters.size() );

        const ParticleEmitterDebug* selected = nullptr;
        if ( ImGui::BeginTable( "##emitters", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg ) )
        {
            for ( const char* header : { "Entity", "Emitter", "Name", "Alive", "Free", "Capacity" } )
                ImGui::TableSetupColumn( header );
            ImGui::TableHeadersRow();
            for ( const ParticleEmitterDebug& e : emitters )
            {
                const bool isSelected = m_Selected && e.EntityId == m_EntityId && e.Emitter == m_Emitter;
                if ( isSelected )
                    selected = &e;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string id = std::to_string( e.EntityId ) + "##" + std::to_string( e.Emitter );
                if ( ImGui::Selectable( id.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns ) )
                {
                    m_Selected = true;
                    m_EntityId = e.EntityId;
                    m_Emitter  = e.Emitter;
                    m_FirstRow = 0;
                }
                ImGui::TableNextColumn();
                ImGui::Text( "%u", e.Emitter );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( e.Name.c_str() );
                ImGui::TableNextColumn();
                if ( e.Read )
                    ImGui::Text( "%u", e.Alive );
                else
                    ImGui::TextUnformatted( "-" );
                ImGui::TableNextColumn();
                if ( e.Read )
                    ImGui::Text( "%u", e.Free );
                else
                    ImGui::TextUnformatted( "-" );
                ImGui::TableNextColumn();
                ImGui::Text( "%u", e.Capacity );
            }
            ImGui::EndTable();
        }

        if ( selected == nullptr )
        {
            ImGui::TextUnformatted( "Select an emitter for its particle attribute table." );
            ImGui::End();
            return;
        }
        ImGui::Separator();
        ImGui::Text( "Attributes of '%s' - alive rows %u..%u of %u (read %u frame(s) after the GPU wrote them)",
                     selected->Name.c_str(), m_FirstRow,
                     m_FirstRow + static_cast<uint32_t>( selected->RowIndex.size() ), selected->Alive,
                     selected->FramesOld );
        if ( ImGui::Button( "< Page" ) && m_FirstRow >= kParticleDebugMaxRows )
            m_FirstRow -= kParticleDebugMaxRows;
        ImGui::SameLine();
        if ( ImGui::Button( "Page >" ) && m_FirstRow + kParticleDebugMaxRows < selected->Alive )
            m_FirstRow += kParticleDebugMaxRows;

        const uint32_t columns = selected->FloatColumns;
        if ( columns > 0 && ImGui::BeginTable( "##attributes", static_cast<int>( columns ) + 1,
                                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY ) )
        {
            ImGui::TableSetupColumn( "Pool index" );
            // One table column per float component, named after its attribute (Position.x, Color.a, ...).
            std::vector<std::string> names( columns );
            for ( const auto& attribute : selected->Columns )
                for ( uint32_t c = 0; c < attribute.FloatCount && attribute.FloatStart + c < columns; ++c )
                    names[attribute.FloatStart + c] =
                         attribute.FloatCount == 1 ? attribute.Name
                                                   : attribute.Name + "." + std::string( 1, "xyzw"[c & 3u] );
            for ( const std::string& name : names )
                ImGui::TableSetupColumn( name.c_str() );
            ImGui::TableHeadersRow();
            for ( std::size_t r = 0; r < selected->RowIndex.size(); ++r )
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text( "%u", selected->RowIndex[r] );
                for ( uint32_t c = 0; c < columns; ++c )
                {
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.3f", selected->Floats[r * columns + c] );
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
} // namespace Desert::Editor

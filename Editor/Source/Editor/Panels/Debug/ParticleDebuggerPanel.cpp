#include "Editor/Panels/Debug/ParticleDebuggerPanel.hpp"

#include <ImGui/imgui.h>
#include <Engine/Desert.hpp>
#include <Engine/Graphic/Systems/Scene/Particles/ParticleWorldGpu.hpp>

#include <string>
#include <string_view>
#include <vector>

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
        m_Frozen   = false;
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
                    m_Frozen   = false; // a frozen page belongs to the previous selection
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
            m_Frozen = false;
            ImGui::TextUnformatted( "Select an emitter for its particle attribute table." );
            ImGui::End();
            return;
        }
        ImGui::Separator();
        // Freeze holds the page last read (UE's captured frame) - the counts above keep updating.
        if ( ImGui::Checkbox( "Freeze", &m_Frozen ) && m_Frozen )
            m_FrozenPage = *selected;
        const ParticleEmitterDebug& page = m_Frozen ? m_FrozenPage : *selected;
        ImGui::SameLine();
        ImGui::Text( "Attributes of '%s' - alive rows %u..%u of %u (read %u frame(s) after the GPU wrote them)%s",
                     page.Name.c_str(), m_FirstRow, m_FirstRow + static_cast<uint32_t>( page.RowIndex.size() ),
                     page.Alive, page.FramesOld, m_Frozen ? " - frozen" : "" );
        ImGui::BeginDisabled( m_Frozen );
        if ( ImGui::Button( "< Page" ) && m_FirstRow >= kParticleDebugMaxRows )
            m_FirstRow -= kParticleDebugMaxRows;
        ImGui::SameLine();
        if ( ImGui::Button( "Page >" ) && m_FirstRow + kParticleDebugMaxRows < page.Alive )
            m_FirstRow += kParticleDebugMaxRows;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 200.0f );
        ImGui::InputTextWithHint( "##filter", "attribute filter", m_Filter, sizeof( m_Filter ) );

        // One table column per component, named after its attribute (Position.x, Color.a, ...): the float
        // components, then the int ones; an attribute whose name does not contain the filter is left out.
        struct Column
        {
            std::string Name;
            bool        Int   = false;
            uint32_t    Index = 0; // the component in Floats / Ints
        };
        std::vector<Column>    shown;
        const std::string_view filter( m_Filter );
        for ( const bool ints : { false, true } )
            for ( const auto& attribute : page.Columns )
            {
                if ( !filter.empty() && attribute.Name.find( filter ) == std::string::npos )
                    continue;
                const uint32_t count = ints ? attribute.IntCount : attribute.FloatCount;
                const uint32_t start = ints ? attribute.IntStart : attribute.FloatStart;
                const uint32_t limit = ints ? page.IntColumns : page.FloatColumns;
                for ( uint32_t c = 0; c < count && start + c < limit; ++c )
                    shown.push_back( { count == 1 ? attribute.Name
                                                  : attribute.Name + "." + std::string( 1, "xyzw"[c & 3u] ),
                                       ints, start + c } );
            }

        if ( !shown.empty() && ImGui::BeginTable( "##attributes", static_cast<int>( shown.size() ) + 1,
                                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                       ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY ) )
        {
            ImGui::TableSetupColumn( "Pool index" );
            for ( const Column& column : shown )
                ImGui::TableSetupColumn( column.Name.c_str() );
            ImGui::TableHeadersRow();
            for ( std::size_t r = 0; r < page.RowIndex.size(); ++r )
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text( "%u", page.RowIndex[r] );
                for ( const Column& column : shown )
                {
                    ImGui::TableNextColumn();
                    if ( column.Int )
                        ImGui::Text( "%d", page.Ints[r * page.IntColumns + column.Index] );
                    else
                        ImGui::Text( "%.3f", page.Floats[r * page.FloatColumns + column.Index] );
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
} // namespace Desert::Editor

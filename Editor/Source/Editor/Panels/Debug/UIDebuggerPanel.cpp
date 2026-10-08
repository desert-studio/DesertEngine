#include "UIDebuggerPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/UIPreview.hpp>
#include <Editor/Core/UIProbeRegistry.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/UI/UIIntrospection.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        void LabelledValue( const char* label, const std::string& value )
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( label );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( value.c_str() );
        }

        std::string U32( std::uint32_t v )
        {
            return std::to_string( v );
        }

        std::string RectText( const ::Desert::UI::Rect& r )
        {
            char buf[96];
            std::snprintf( buf, sizeof( buf ), "x %.1f  y %.1f  w %.1f  h %.1f", r.X, r.Y, r.W, r.H );
            return buf;
        }

        // The inherited clip, both halves. The box is what the scissor cut; the plane count is what a
        // rotated clipper cut on top of it, and printing only the box would make a turned clipper look
        // identical to the straight one whose box it shares — which is exactly the confusion this panel
        // exists to remove.
        std::string ClipRegionText( const ::Desert::Graphic::Render2D::ClipRegion2D& r )
        {
            if ( !r.Bounded )
                return "unclipped";
            if ( ::Desert::Graphic::Render2D::ClipRegionEmpty( r ) )
                return "empty — nothing survives it";
            char buf[128];
            std::snprintf( buf, sizeof( buf ), "x %.1f  y %.1f  w %.1f  h %.1f", r.Box.x, r.Box.y, r.Box.z,
                           r.Box.w );
            std::string text = buf;
            if ( r.PlaneCount > 0 )
                text += "  + " + std::to_string( r.PlaneCount ) + " oblique edge(s)";
            return text;
        }

        std::string Vec2Text( const glm::vec2& v )
        {
            char buf[64];
            std::snprintf( buf, sizeof( buf ), "%.3f, %.3f", v.x, v.y );
            return buf;
        }

        std::string TextureText( const void* texture )
        {
            if ( texture == nullptr )
                return "white (solid)";
            char buf[32];
            std::snprintf( buf, sizeof( buf ), "%p", texture );
            return buf;
        }

        const char* HitTestName( ::Desert::UI::UIHitTest h )
        {
            switch ( h )
            {
                case ::Desert::UI::UIHitTest::All:
                    return "All";
                case ::Desert::UI::UIHitTest::ChildrenOnly:
                    return "ChildrenOnly";
                case ::Desert::UI::UIHitTest::Blocking:
                    return "Blocking";
                case ::Desert::UI::UIHitTest::None:
                    return "None";
            }
            return "?";
        }

        // Two-column key/value table, the shape the Details panel uses for read-only facts.
        bool BeginFacts( const char* id )
        {
            return ImGui::BeginTable( id, 2,
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_BordersInnerV );
        }
    } // namespace

    UIDebuggerPanel::UIDebuggerPanel( std::shared_ptr<::Desert::Core::Scene> scene )
         : IPanel( "UI Debugger", /*showPanel=*/false ), m_Scene( std::move( scene ) )
    {
    }

    UIDebuggerPanel::~UIDebuggerPanel()
    {
        // Closing the editor with the panel open must not leave the render pass capturing forever.
        if ( m_Arming )
            Core::UIProbeRegistry::Get().SetArmed( false );
    }

    void UIDebuggerPanel::OnPreUpdate()
    {
        // THE ARM AND THE DISARM BOTH LIVE HERE, and that is why it is not done in OnUIRender: OnUIRender
        // is not called while the window is closed, so a probe armed there could never be switched off
        // again — it would keep capturing for the life of the editor, which is exactly the "collects only
        // when the panel is open" requirement failing quietly. OnPreUpdate runs every frame regardless,
        // and it runs BEFORE the scene is drawn, so the first open frame is already captured.
        const bool open = GetVisibility();
        if ( open == m_Arming )
            return;
        m_Arming = open;
        Core::UIProbeRegistry::Get().SetArmed( open );
    }

    void UIDebuggerPanel::OnUIRender()
    {
        if ( !m_Scene )
        {
            Utils::ImGuiUtilities::EmptyState( ICON_MDI_VIEW_DASHBOARD_OUTLINE, "No scene",
                                               "Open a scene view to inspect its UI canvas." );
            return;
        }

        const auto* sink = Core::UIProbeRegistry::Get().Find( m_Scene.get() );
        if ( sink == nullptr || sink->Captures() == 0 )
        {
            Utils::ImGuiUtilities::EmptyState( ICON_MDI_TIMER_SAND, "Waiting for a frame",
                                               "The scene's UI pass has not drawn since this panel was opened." );
            return;
        }

        const ::Desert::UI::UIFrameProbe& probe = sink->Frame();
        if ( !probe.Valid )
        {
            // The pass captured and the canvas refused. Say which refusal — "no canvas", "two canvases"
            // and "the canvas is switched off" are three different problems with three different fixes.
            Utils::ImGuiUtilities::EmptyState( ICON_MDI_ALERT_OUTLINE, "No canvas to measure",
                                               probe.Refusal.c_str() );
            return;
        }

        const auto& stats = probe.Stats;
        const auto& walk  = probe.Walk;

        // --- The frame ---------------------------------------------------------------------------------
        {
            char detail[96];
            std::snprintf( detail, sizeof( detail ), "%u batches  %u draws", stats.Batches, stats.DrawCalls );
            if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_CHART_BAR "  Frame", true, detail ) )
            {
                ImGui::Indent();
                if ( BeginFacts( "##uidbg_frame" ) )
                {
                    LabelledValue( "Batches recorded", U32( stats.Batches ) );
                    LabelledValue( "Draw calls submitted", U32( stats.DrawCalls ) );
                    // Only when it is non-zero. No DrawList2D primitive can open a command and then
                    // append nothing (AddRing refuses before opening one), so Render2D::Flush's
                    // IndexCount == 0 guard is unreachable today and this row would be a permanent
                    // "0" — a column that answers nobody's question. Its silence is the answer, and
                    // Desert/Tests/Engine/UIIntrospection is what makes that silence mean something.
                    if ( stats.EmptyBatches > 0 )
                        LabelledValue( "Batches with no geometry", U32( stats.EmptyBatches ) );
                    LabelledValue( "Pipeline switches", U32( stats.PipelineSwitches ) );
                    LabelledValue( "Distinct textures", U32( stats.UniqueTextures ) );
                    // Shown only when the canvas uses one, on the same rule as the empty-batch row two
                    // lines up: a permanent "0" beside every canvas that has no material is a column
                    // answering nobody's question. When it is there it is the expensive one — a material
                    // change is a PIPELINE bind, where a texture change is only a descriptor bind.
                    if ( stats.UniqueMaterials > 0 )
                        LabelledValue( "Distinct UI materials", U32( stats.UniqueMaterials ) );
                    LabelledValue( "Vertices", U32( stats.Vertices ) );
                    LabelledValue( "Triangles", U32( stats.Triangles ) );
                    LabelledValue( "Largest batch (triangles)", U32( stats.LargestBatchTris ) );
                    ImGui::EndTable();
                }
                ImGui::Unindent();
            }
        }

        // --- Why the batches broke ---------------------------------------------------------------------
        // The headline of the whole panel: a batch count nobody can act on becomes a reason somebody can.
        {
            if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_CALL_SPLIT "  Why each batch opened" ) )
            {
                ImGui::Indent();
                if ( BeginFacts( "##uidbg_breaks" ) )
                {
                    for ( std::size_t i = 0; i < ::Desert::UI::kBatchBreakCount; ++i )
                    {
                        const auto reason = static_cast<::Desert::UI::BatchBreak>( i );
                        if ( reason == ::Desert::UI::BatchBreak::None || stats.BreakCounts[i] == 0 )
                            continue; // a reason that did not fire is not a row: no dead columns
                        LabelledValue( ::Desert::UI::BatchBreakName( reason ), U32( stats.BreakCounts[i] ) );
                    }
                    ImGui::EndTable();
                }

                ImGui::Checkbox( "List every batch", &m_ShowBatchList );
                if ( m_ShowBatchList &&
                     ImGui::BeginTable( "##uidbg_batchlist", 5,
                                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                        ImVec2( 0.0f, 180.0f ) ) )
                {
                    ImGui::TableSetupScrollFreeze( 0, 1 );
                    ImGui::TableSetupColumn( "#" );
                    ImGui::TableSetupColumn( "Opened by" );
                    ImGui::TableSetupColumn( "Texture" );
                    ImGui::TableSetupColumn( "Kind" );
                    ImGui::TableSetupColumn( "Tris" );
                    ImGui::TableHeadersRow();
                    for ( const ::Desert::UI::UIBatchInfo& b : probe.Batches )
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text( "%u", b.Index );
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted( ::Desert::UI::BatchBreakName( b.Break ) );
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted( TextureText( b.Texture ).c_str() );
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted( b.Glass ? "glass" : b.Text ? "text" : "solid" );
                        ImGui::TableNextColumn();
                        ImGui::Text( "%u", b.IndexCount / 3 );
                    }
                    ImGui::EndTable();
                }
                ImGui::Unindent();
            }
        }

        // --- The walk ----------------------------------------------------------------------------------
        {
            char detail[96];
            std::snprintf( detail, sizeof( detail ), "%u drawn / %u", walk.Drawn, walk.Visited );
            if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FILE_TREE "  Elements", true, detail ) )
            {
                ImGui::Indent();
                if ( BeginFacts( "##uidbg_walk" ) )
                {
                    LabelledValue( "Visited", U32( walk.Visited ) );
                    LabelledValue( "Drawn", U32( walk.Drawn ) );
                    LabelledValue( "Skipped", U32( walk.Skipped ) );
                    LabelledValue( "Drawn but fully clipped", U32( walk.Clipped ) );
                    LabelledValue( "Deepest nesting", U32( walk.MaxDepth ) );
                    for ( std::size_t i = 0; i < walk.SkipCounts.size(); ++i )
                    {
                        if ( walk.SkipCounts[i] == 0 )
                            continue;
                        LabelledValue(
                             ::Desert::UI::UISkipCauseName( static_cast<::Desert::UI::UISkipCause>( i ) ),
                             U32( walk.SkipCounts[i] ) );
                    }
                    ImGui::EndTable();
                }
                ImGui::Unindent();
            }
        }

        // --- The selected element ----------------------------------------------------------------------
        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
        {
            ImGui::TextDisabled( "Select a UI element to inspect it." );
            return;
        }
        const auto found = m_Scene->FindEntityByID( *selected );
        if ( !found.has_value() )
        {
            ImGui::TextDisabled( "The selected entity is not in this scene." );
            return;
        }
        const entt::entity handle = found->get().GetHandle();

        const auto node =
             std::find_if( probe.Elements.begin(), probe.Elements.end(),
                           [handle]( const ::Desert::UI::UIElementNode& n ) { return n.Entity == handle; } );
        if ( node == probe.Elements.end() )
        {
            ImGui::TextDisabled( "The selected entity is not under this canvas." );
            return;
        }

        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_CURSOR_DEFAULT_CLICK "  Selected element", true,
                                                   node->Drawn ? "drawn" : "not drawn" ) )
        {
            ImGui::Indent();
            if ( BeginFacts( "##uidbg_element" ) )
            {
                LabelledValue( "Drawn", node->Drawn ? "yes" : "no" );
                if ( !node->Drawn )
                {
                    LabelledValue( "Not drawn because", ::Desert::UI::UISkipCauseName( node->Cause ) );
                    // OWN OR INHERITED, said as a fact rather than left to be worked out: "this element is
                    // Hidden" and "a panel four levels up is Hidden" look identical on screen and need
                    // opposite fixes.
                    LabelledValue( "Stopped by",
                                   node->CauseBy == node->Entity
                                        ? std::string( "itself" )
                                        : "entity " + U32( static_cast<std::uint32_t>( node->CauseBy ) ) );
                }
                LabelledValue( "Draw order", node->Drawn ? U32( static_cast<std::uint32_t>( node->Order ) )
                                                         : std::string( "-" ) );
                LabelledValue( "Depth", U32( static_cast<std::uint32_t>( node->Depth ) ) );
                LabelledValue( "Rect (px, pre-transform)",
                               node->RectValid ? RectText( node->RectPx )
                                               : std::string( "none — a layout group left it no slot" ) );
                LabelledValue( "On screen (px)", RectText( node->ScreenPx ) );
                LabelledValue( "Clip inherited (px)", ClipRegionText( node->ClipRegion ) );
                LabelledValue( "Pixels it may occupy", node->Clipped
                                                            ? std::string( "none — clipped away entirely" )
                                                            : RectText( node->VisiblePx ) );
                LabelledValue( "Clips its children", node->ClipsChildren ? "yes" : "no" );
                LabelledValue( "Takes a layout slot", node->TakesSlot ? "yes" : "no" );
                LabelledValue( "Hit test", HitTestName( node->HitTest ) );
                LabelledValue( "Pointer can stop here", node->ElectsSelf ? "yes"
                                                                         : "no (an ancestor or its "
                                                                           "own Hit Test forbids it)" );

                // KEYBOARD FOCUS, and only while UI Preview is running. Focus is moved by Tab and read by
                // Enter, and both only exist in preview — in Design mode the row would read "no" for every
                // element forever, which is a column that answers nobody. UE's Widget Reflector shows the
                // same fact in the same place (its Focus column) for the same reason: "the pointer is over
                // this control and Enter went somewhere else" is otherwise unanswerable.
                if ( Core::UIPreview::Get().Enabled )
                    LabelledValue( "Keyboard focus", Core::UIPreview::Get().Focused == handle ? "yes" : "no" );

                auto& reg = m_Scene->GetRegistry();
                if ( reg.has<ECS::UILayoutComponent>( handle ) )
                {
                    const auto& L = reg.get<ECS::UILayoutComponent>( handle ).Data;
                    LabelledValue( "Anchor min / max", Vec2Text( L.AnchorMin ) + "   " + Vec2Text( L.AnchorMax ) );
                    LabelledValue( "Offset min / max", Vec2Text( L.OffsetMin ) + "   " + Vec2Text( L.OffsetMax ) );
                    LabelledValue( "Pivot", Vec2Text( L.Pivot ) );
                    LabelledValue( "Rotation / scale",
                                   std::to_string( L.Rotation ) + " deg   " + Vec2Text( L.Scale ) );
                }
                ImGui::EndTable();
            }

            // --- The measurement -----------------------------------------------------------------------
            // Asked ONCE PER SELECTION, not once per frame: it costs two extra walks of the canvas, and
            // the question ("what does THIS element cost") is asked by picking the element. Asking it
            // every frame would charge the panel two walks a frame for an answer that only changes when
            // the canvas is edited — which is what the button is for.
            if ( handle != m_Measured )
            {
                m_Measured = handle;
                Core::UIProbeRegistry::Get().Slot( m_Scene.get() ).RequestElementCost( handle );
            }
            if ( ImGui::Button( "Re-measure" ) )
                Core::UIProbeRegistry::Get().Slot( m_Scene.get() ).RequestElementCost( handle );

            const auto& cost = sink->ElementCost();
            if ( sink->CostSubject() == handle )
            {
                if ( !cost.Valid )
                {
                    ImGui::TextDisabled( "%s", cost.Refusal.c_str() );
                }
                else if ( BeginFacts( "##uidbg_cost" ) )
                {
                    LabelledValue( "Vertices it contributes", U32( cost.Vertices ) );
                    LabelledValue( "Triangles it contributes", U32( cost.Triangles ) );
                    LabelledValue( "Canvas batches with it", U32( cost.BatchesWith ) );
                    LabelledValue( "Canvas batches without it", U32( cost.BatchesWithout ) );
                    if ( cost.Indices > 0 )
                    {
                        LabelledValue( "Lands in batch", U32( cost.FirstBatch ) );
                        LabelledValue( "That batch's texture", TextureText( cost.Texture ) );
                        LabelledValue( "That batch was opened by", ::Desert::UI::BatchBreakName( cost.Break ) );
                    }
                    LabelledValue( "Breaks the canvas apart",
                                   cost.OpensBatch ? "yes — hiding it removes a batch" : "no" );
                    ImGui::EndTable();
                }
            }
            ImGui::Unindent();
        }
    }
} // namespace Desert::Editor

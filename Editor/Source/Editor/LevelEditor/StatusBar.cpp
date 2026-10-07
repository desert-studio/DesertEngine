#include "Editor/LevelEditor/StatusBar.hpp"

#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/GizmoState.hpp"
#include "Editor/Core/MeshResolve.hpp"
#include "Editor/Core/NumberFormat.hpp"
#include "Editor/Core/PanelRequests.hpp"
#include "Editor/Core/Selection/SelectionManager.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/Import/BackgroundCook.hpp"
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/Panels/Logs/LogsPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include <Common/Core/Version.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/ViewBudget.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/MeshStats.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace Desert::Editor
{
    uint64_t StatusBar::SceneTriangleCount()
    {
        const uint64_t revision    = CommandHistory::Get().Revision();
        const size_t   entityCount = m_Workspace.ActiveScene()->GetAllEntities().size();

        // Recompute on an edit, on the population changing, or every ~120 frames — the last one because an
        // async mesh load completes without touching either of the other two, and a status bar stuck on
        // "0 tris" while the scene is visibly full would be worse than showing no number at all.
        constexpr int kMaxCacheAgeFrames = 120;
        if ( revision == m_TriangleCacheRev && entityCount == m_TriangleCacheCount &&
             ++m_TriangleCacheAge < kMaxCacheAgeFrames )
        {
            return m_TriangleCache;
        }

        uint64_t total = 0;
        for ( const ECS::Entity& entity : m_Workspace.ActiveScene()->GetAllEntities() )
        {
            // HIDDEN entities are excluded: the number sits beside the entity count in a bar that answers
            // "what is on screen", and a hidden mesh is not.
            if ( entity.HasComponent<ECS::VisibilityComponent>() &&
                 !entity.GetComponent<ECS::VisibilityComponent>().Visible )
            {
                continue;
            }
            // ResolveDrawnMesh, not the mesh handle: a primitive draws the process-wide shared mesh and has
            // no handle at all, and counting only handles reports zero for a scene of cubes (the exact trap
            // that helper documents).
            if ( const ::Desert::Mesh* mesh = ResolveDrawnMesh( entity ) )
                total += Geometry::ComputeMeshStats( mesh->GetSubmeshes() ).Triangles;
        }

        m_TriangleCache      = total;
        m_TriangleCacheRev   = revision;
        m_TriangleCacheCount = entityCount;
        m_TriangleCacheAge   = 0;
        return total;
    }

    void StatusBar::Draw()
    {
        namespace ImGui  = ::ImGui;
        using SceneState = ::Desert::Core::Scene::SceneState;

        const auto  state     = m_Workspace.ActiveScene()->GetState();
        const char* stateText = ICON_MDI_PENCIL " Edit";
        if ( state == SceneState::Play )
            stateText = ICON_MDI_PLAY " Play";
        else if ( state == SceneState::Paused )
            stateText = ICON_MDI_PAUSE " Paused";
        const ImVec4 stateColor =
             ( state == SceneState::Edit ) ? ThemeManager::GetIconColor() : ThemeManager::GetSelectedColor();

        ImGui::PushStyleColor( ImGuiCol_ChildBg, ImVec4( 0.086f, 0.086f, 0.086f, 1.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8.0f, 2.0f ) );
        ImGui::BeginChild( "##StatusBar", ImVec2( 0.0f, 0.0f ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );

        // The "Content Drawer" / "Output Log" buttons that used to live here are gone. They duplicated
        // the Assets and Logs panels that are already docked along the bottom — two ways to reach one
        // thing, and the button version could only toggle a panel out of existence. What is left is a
        // single chevron that COLLAPSES that bottom drawer instead: a closed panel has to be
        // rediscovered from a menu, a collapsed one is still right there with its tabs visible.
        m_DrawBottomDrawerToggle();
        ImGui::SameLine( 0.0f, 12.0f );

        if ( m_BackgroundCook && m_BackgroundCook->Outstanding() > 0 )
        {
            ImGui::TextDisabled( ICON_MDI_COG " Cooking %zu of %zu mesh source(s)",
                                 m_BackgroundCook->Outstanding(), m_BackgroundCook->Total() );
            ImGui::SameLine( 0.0f, 12.0f );
        }

        // Cmd: one line of Lua against the live scene, the same engine the Lua Console runs. UE puts a
        // console here for the same reason — a question about the running world should not need a panel.
        ImGui::TextDisabled( ICON_MDI_CONSOLE );
        ImGui::SameLine( 0.0f, 4.0f );
        ImGui::SetNextItemWidth( 220.0f );
        if ( ImGui::InputTextWithHint( "##StatusCmd", "Enter Console Command", m_StatusCmd, sizeof( m_StatusCmd ),
                                       ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            if ( m_StatusCmd[0] != '\0' )
            {
                Core::PanelRequests::Open( "Lua Console" );
                LuaConsolePanel::Submit( m_StatusCmd );
                m_StatusCmd[0] = '\0';
            }
        }
        ImGui::SameLine( 0.0f, 16.0f );

        // Then: scene state + current selection.
        ImGui::PushStyleColor( ImGuiCol_Text, stateColor );
        ImGui::TextUnformatted( stateText );
        ImGui::PopStyleColor();

        // (The scene name + dirty marker moved UP into the window toolbar breadcrumb.)
        ImGui::SameLine( 0.0f, 16.0f );
        if ( const size_t selCount = Core::SelectionManager::Count(); selCount > 1 )
        {
            ImGui::TextDisabled( ICON_MDI_CURSOR_DEFAULT_OUTLINE " %zu selected", selCount );
        }
        else if ( const auto sel = Core::SelectionManager::GetSelected() )
        {
            std::string selName = "Entity";
            if ( auto e = m_Workspace.ActiveScene()->FindEntityByID( *sel ) )
                selName = e->get().GetComponent<ECS::TagComponent>().Tag;
            ImGui::TextDisabled( ICON_MDI_CURSOR_DEFAULT_OUTLINE " %s", selName.c_str() );
        }
        else
        {
            ImGui::TextDisabled( "No selection" );
        }

        ImGui::SameLine( 0.0f, 16.0f );
        ImGui::TextDisabled( ICON_MDI_SHAPE " %zu entities", m_Workspace.ActiveScene()->GetAllEntities().size() );

        // What the scene costs to draw, beside what it contains. Two numbers that belong together: an
        // entity count says how much there is to manage, a triangle count says how much there is to
        // render, and only the second one explains a frame time.
        ImGui::SameLine( 0.0f, 16.0f );
        {
            const uint64_t tris = SceneTriangleCount();
            ImGui::TextDisabled( ICON_MDI_TRIANGLE_OUTLINE " %s tris", FormatThousands( tris ).c_str() );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Triangles in the VISIBLE meshes of this scene (LOD 0)." );
        }

        // HOW MANY DOCUMENTS, AND HOW MUCH OF THE VIEW BUDGET IS GONE. A count of documents is not the number
        // that matters; the memory the views hold against the device-local budget is, which is why they are
        // shown together: three documents can be three views or none, depending on which three.
        ImGui::SameLine( 0.0f, 16.0f );
        {
            uint64_t held = 0;
            for ( const Engine::ViewBudget::HeldView& view : Graphic::SceneRenderer::LiveHoldings() )
                held += view.Bytes;
            const uint64_t                    pending = PendingViewBytes( m_Documents.Documents().Documents() );
            const Engine::ViewBudget::Reading reading = Graphic::ReadViewBudget();

            // Warned past 90 %: the next document is likely refused, and the user should see that coming
            // before the click rather than after it. ImGuiCol_TextDisabled, not ImGuiCol_Text: the line
            // below is drawn with TextDisabled like the rest of the bar.
            const bool tight =
                 ( reading.UsageBytes + pending ) * 10 > reading.CeilingBytes * 9 && reading.CeilingBytes != 0;
            if ( tight )
                ImGui::PushStyleColor( ImGuiCol_TextDisabled, ThemeManager::GetWarningColor() );
            ImGui::TextDisabled( ICON_MDI_FILE_DOCUMENT_MULTIPLE_OUTLINE " %zu document%s \xc2\xb7 %s / %s",
                                 m_Documents.Documents().Count(), m_Documents.Documents().Count() == 1 ? "" : "s",
                                 Engine::ViewBudget::FormatMiB( held ).c_str(),
                                 Engine::ViewBudget::FormatMiB( reading.CeilingBytes ).c_str() );
            if ( tight )
                ImGui::PopStyleColor();

            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip(
                     "%zu open document(s). Open views hold %s; %s; in use %s; %s more is spoken for "
                     "by documents that have not drawn yet. A document whose view does not fit is "
                     "refused.",
                     m_Documents.Documents().Count(), Engine::ViewBudget::FormatMiB( held ).c_str(),
                     Engine::ViewBudget::DescribeCeiling( reading ).c_str(),
                     Engine::ViewBudget::FormatMiB( reading.UsageBytes ).c_str(),
                     Engine::ViewBudget::FormatMiB( pending ).c_str() );
        }

        // Active snap state: off, or the step of the CURRENT transform tool — answers "why did it
        // jump?" without opening the snap popup.
        ImGui::SameLine( 0.0f, 16.0f );
        {
            using Gz = ::Desert::Editor::Core::GizmoState;
            if ( !Gz::PersistentSnap() )
                ImGui::TextDisabled( ICON_MDI_MAGNET " off" );
            else
                switch ( Gz::Get() )
                {
                    case Gz::Operation::Rotate:
                        ImGui::TextDisabled( ICON_MDI_MAGNET " %.1f\xC2\xB0", Gz::RotateSnapDegrees() );
                        break;
                    case Gz::Operation::Scale:
                        ImGui::TextDisabled( ICON_MDI_MAGNET " x%.2f", Gz::ScaleSnap() );
                        break;
                    default:
                        // CENTIMETRES, and metres only past a metre — the same rule DrawSnapPopup
                        // formats the toolbar button with, and it has to be the same rule because the two
                        // labels sit on one screen reading one value. This said "%.2fm" over a value that
                        // is in world units (1 unit = 1 cm), so a 5 m step read "500.00m" three inches
                        // from a button reading "5 m". Third sighting of У5's metre-era label: the field's
                        // default, the Preferences slider, and now the status bar.
                        if ( Gz::TranslateSnap() >= 100.0f )
                            ImGui::TextDisabled( ICON_MDI_MAGNET " %.0f m", Gz::TranslateSnap() / 100.0f );
                        else
                            ImGui::TextDisabled( ICON_MDI_MAGNET " %.0f cm", Gz::TranslateSnap() );
                        break;
                }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Snap (toggle in the viewport toolbar; Ctrl inverts while dragging)" );
        }

        // Right: how much the log is complaining, then which build this is.
#ifdef DESERT_CONFIG_DEBUG
        constexpr const char* kBuildConfig = "Debug";
#else
        constexpr const char* kBuildConfig = "Release";
#endif
        const float fps = ImGui::GetIO().Framerate;
        // The BRANCH beside the version: this project runs eight worktrees at once, and "which of these
        // windows is my build" was previously answerable only from the commit hash. It comes from the same
        // build-time git identity the version does, so it cannot disagree with the hash beside it.
        char stats[220];
        std::snprintf( stats, sizeof( stats ),
                       ICON_MDI_SOURCE_BRANCH " %s   %s  %s   " ICON_MDI_SPEEDOMETER " %.0f FPS   %.2f ms",
                       Common::Version::Branch(), Common::Version::Full(), kBuildConfig, fps,
                       fps > 0.0f ? 1000.0f / fps : 0.0f );

        // "Are there warnings?" answered where you are already looking, without opening the log. The count
        // comes from the Logs panel's parse of the file — the one place that has read it — so the chip in
        // that panel and this number cannot disagree.
        const std::size_t warnings   = LogsPanel::WarningCount();
        const std::size_t errors     = LogsPanel::ErrorCount();
        char              alerts[96] = {};
        if ( errors > 0 )
            std::snprintf( alerts, sizeof( alerts ), ICON_MDI_CLOSE_CIRCLE_OUTLINE " %zu   " ICON_MDI_ALERT " %zu",
                           errors, warnings );
        else if ( warnings > 0 )
            std::snprintf( alerts, sizeof( alerts ), ICON_MDI_ALERT " %zu warnings", warnings );

        const bool  dirty   = m_SceneFiles.HasUnsavedChanges();
        const float starW   = dirty ? ImGui::CalcTextSize( "* " ).x : 0.0f;
        const float statsW  = ImGui::CalcTextSize( stats ).x;
        const float alertsW = alerts[0] != '\0' ? ImGui::CalcTextSize( alerts ).x + 16.0f : 0.0f;
        // Right-aligned, but never left of where the left half actually ended: the document/budget text
        // grows with its numbers, and a position computed from the right edge alone drew the counters on
        // top of it. When both halves do not fit, the right half is pushed out and clipped, not overlaid.
        const float leftEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 16.0f;
        ImGui::SameLine( std::max( leftEndX, ImGui::GetWindowContentRegionMax().x - statsW - starW - alertsW ) );

        if ( alerts[0] != '\0' )
        {
            // Errors outrank warnings in the colour as well as in the text: one red count is the whole
            // signal, and painting it amber because warnings are also present would bury it.
            ImGui::TextColored( errors > 0 ? ThemeManager::GetErrorColor() : ThemeManager::GetWarningColor(), "%s",
                                alerts );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%zu error(s), %zu warning(s) in this session's log — click to open it.",
                                   errors, warnings );
            if ( ImGui::IsItemClicked() )
                Core::PanelRequests::Open( "Logs" );
            ImGui::SameLine( 0.0f, 16.0f );
        }

        if ( dirty )
        {
            // Amber star next to the version/config block = unsaved scene changes.
            ImGui::TextColored( ThemeManager::GetWarningColor(), "*" );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Unsaved changes (Ctrl+S to save)" );
            ImGui::SameLine( 0.0f, ImGui::CalcTextSize( " " ).x );
        }
        ImGui::TextDisabled( "%s", stats );

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
} // namespace Desert::Editor

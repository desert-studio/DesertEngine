#include "Editor/LevelEditor/DockLayout.hpp"

#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/Core/IconsMaterialDesignIcons.hpp"
#include "Editor/Core/LayoutManager.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/Core/PanelRequests.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/WindowTitles.hpp"
#include "Editor/Panels/IPanel.hpp"
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Core/Application.hpp>
#include <ImGui/imgui_internal.h>

#include <string>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    DockLayout::DockLayout( PanelRegistry& panels, DocumentHost& documents, SceneWorkspace& workspace,
                            SceneFiles& sceneFiles )
         : m_Panels( panels ), m_Documents( documents ), m_Workspace( workspace ), m_SceneFiles( sceneFiles )
    {
    }

    Common::BoolResultStr DockLayout::BindLayoutFile()
    {
        ImGuiIO& io = ::ImGui::GetIO();
        // THE DOCKING LAYOUT FILE, OFF THE PROJECT (UE: <Project>/Saved/Config/EditorLayout.ini) — never ImGui's
        // default "imgui.ini", which fopen resolves against the working directory: an editor started from /tmp
        // read and wrote /tmp/imgui.ini and came up with a different layout than one started from Editor/.
        // Static storage: io.IniFilename is a borrowed C string ImGui reads at the first frame and on every save.
        {
            static std::string          s_LayoutIni;
            const std::filesystem::path configDir = Common::Constants::Path::ProjectDir() / "Saved" / "Config";
            std::error_code             dirError;
            std::filesystem::create_directories( configDir, dirError );
            if ( dirError )
                return Common::MakeFormattedError( "the editor layout folder '{}' could not be created: {}",
                                                   configDir.string(), dirError.message() );
            s_LayoutIni    = ( configDir / "EditorLayout.ini" ).string();
            io.IniFilename = s_LayoutIni.c_str();
        }

        return BOOLSUCCESS;
    }

    void DockLayout::OfferRecovery( std::filesystem::path autosave )
    {
        m_RecoveryAutosave   = std::move( autosave );
        m_ShowRecoveryPrompt = !m_RecoveryAutosave.empty();
    }

    void DockLayout::RequestSaveLayoutAs()
    {
        m_LayoutNameBuf[0]    = '\0';
        m_ShowSaveLayoutPopup = true;
    }

    void DockLayout::UpdateContextualPanels()
    {
        for ( auto& panel : m_Panels )
        {
            // An EXPLICIT request always wins and applies to every panel, contextual or not: a button in
            // Details ("Anim Layers") asked for this panel BY NAME. It pins it, exactly like ticking it in
            // the View menu — the user asked, so nothing auto-closes it.
            //
            // BY NAME IS ALL A TOOL CAN BE ASKED FOR, and that is why the Anim Graph, the Particle Editor,
            // the UI Editor and the Sequencer no longer come through here: "show the one Sequencer window"
            // was the most their Details buttons could say, and the window then had to guess which rig it
            // was about from the selection. They ask for a SUBJECT now
            // (Core::SubjectOpenRequests::Request), which is a different wire because it carries what to
            // edit — see Editor/Core/SubjectOpenRequest.hpp.
            switch ( Core::PanelRequests::Consume( panel->GetName() ) )
            {
                case Core::PanelRequests::Action::Open:
                    panel->GetVisibility() = true;
                    panel->Pinned()        = true;
                    m_FocusPanel           = panel->GetName();
                    break;

                // A drawer button is a switch, not a summons: pressing it again puts the panel away.
                case Core::PanelRequests::Action::Toggle:
                    panel->GetVisibility() = !panel->GetVisibility();
                    panel->Pinned()        = panel->GetVisibility();
                    if ( panel->GetVisibility() )
                        m_FocusPanel = panel->GetName();
                    break;

                case Core::PanelRequests::Action::None:
                    break;
            }

            if ( !panel->IsContextual() )
                continue;

            const bool relevant = panel->IsRelevant();
            bool&      visible  = panel->GetVisibility();

            // Pinning is set ONLY where the user actually asks for the panel (View menu / command
            // palette). Inferring it from "visible but not relevant" also fired on the very first frame
            // for a panel that merely starts visible, pinning it open forever.
            if ( relevant && !visible && !panel->Pinned() )
            {
                visible      = true;
                m_FocusPanel = panel->GetName(); // bring it forward in whatever dock it lives
            }
            else if ( !relevant && visible && !panel->Pinned() )
            {
                visible = false;
            }
            else if ( !visible )
            {
                panel->Pinned() = false; // closed by hand -> stop pinning it open
            }
        }
    }

    void DockLayout::BeginHost()
    {
        // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
        // because it would be confusing to have two docking targets within each others.
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

        if ( m_Fullscreen )
        {
            const ImGuiViewport* viewport = ::ImGui::GetMainViewport();

            auto pos     = viewport->Pos;
            auto size    = viewport->Size;
            bool menuBar = true;
            if ( menuBar )
            {
                const float infoBarSize = ::ImGui::GetFrameHeight();
                pos.y += infoBarSize;
                size.y -= infoBarSize;
            }

            ::ImGui::SetNextWindowPos( pos );
            ::ImGui::SetNextWindowSize( size );
            ::ImGui::SetNextWindowViewport( viewport->ID );

            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 0.0f );
            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        }

        // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
        // and handle the pass-thru hole, so we ask Begin() to not render a background.
        if ( m_DockspaceFlags & ImGuiDockNodeFlags_DockSpace )
            window_flags |= ImGuiWindowFlags_NoBackground;

        // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
        // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
        // all active windows docked into it will lose their parent and become undocked.
        // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
        // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
        ::ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
        ::ImGui::Begin( "DockSpace Demo", &m_HostOpen, window_flags );
        ::ImGui::PopStyleVar();

        if ( m_Fullscreen )
            ::ImGui::PopStyleVar( 2 );
    }

    void DockLayout::DrawDockSpace()
    {
        // Submit the DockSpace
        ImGuiIO& io = ::ImGui::GetIO();

        if ( io.ConfigFlags & ImGuiConfigFlags_DockingEnable )
        {
            // Reserve the bottom status-bar height so the DockSpace fills only the area between the toolbar
            // and the status bar (a full-height DockSpace(0,0) would sit under the status bar).
            // UE's major tabs: "Scene | <asset>" above everything, each asset editor owning the whole area.
            m_Documents.DrawMajorTabStrip();
            const float statusBarHeight = ::ImGui::GetFrameHeight() + 4.0f;
            ImVec2      dockSize        = ::ImGui::GetContentRegionAvail();
            dockSize.y                  = ( dockSize.y > statusBarHeight ) ? dockSize.y - statusBarHeight : 0.0f;

            ImGuiID dockspace_id = ::ImGui::GetID( "MyDockSpace" );

            // One-time auto-relayout: when the default layout's window IDs change (panel-title icons add a
            // ### suffix, changing every window's ImGui ID), old imgui.ini bindings stop matching and panels
            // scatter. Bump kDockLayoutVersion to force a single clean rebuild for everyone, then persist it.
            // 3: the centre is split and documents get a node of their own (layout option B.1).
            // 4: the centre is the level's alone again; documents open as tabs beside it (DocumentPlacement)
            //    and the Documents index moves to the bottom drawer.
            constexpr int kDockLayoutVersion = 4;
            if ( EditorPreferences::Get().DockLayoutVersion < kDockLayoutVersion )
            {
                // SAID OUT LOUD. Every existing imgui.ini is rebuilt once here, and a layout that changes
                // in silence is read as the editor having lost the user's panels — which is the same
                // complaint an area that collapses on its own produces, and the reason B.1 does not
                // collapse. One line naming the old and new versions is the difference between "my layout
                // was reset by the update" and "my layout is gone".
                LOG_INFO( "[Editor] Docking layout rebuilt once: saved layout is version {}, this build lays "
                          "out version {} (asset documents now open as tabs beside the level viewport, and the "
                          "Documents index is a tab in the bottom drawer). Your named layouts under View -> "
                          "Layouts are untouched.",
                          EditorPreferences::Get().DockLayoutVersion, kDockLayoutVersion );

                m_ResetDefaultLayout = true;

                // SaveMigrated, NOT Save: nothing the user did triggered this write. It fires on the
                // first frame after an update, because a stored layout version is being raised to the
                // one this build lays out — the same shape as the metre-era grid snap Load() raises, and
                // the same reason it must be written back (a version that did not persist would rebuild
                // the layout on every launch). Save() means "the user changed a setting" and would have
                // logged this one as if they had.
                const std::string layoutVersions = std::to_string( EditorPreferences::Get().DockLayoutVersion ) +
                                                   " -> " + std::to_string( kDockLayoutVersion );

                EditorPreferences::Get().DockLayoutVersion = kDockLayoutVersion;
                EditorPreferences::SaveMigrated( "docking layout version " + layoutVersions );
            }

            // First run (nothing saved in imgui.ini for this dockspace): lay the panels
            // out into a sensible default instead of leaving them floating in a pile.
            // Checked BEFORE DockSpace() — the call itself creates the node. "Reset to Default
            // Layout" (View -> Layouts) forces the same rebuild on demand.
            const bool buildDefaultLayout =
                 ::ImGui::DockBuilderGetNode( dockspace_id ) == nullptr || m_ResetDefaultLayout;
            m_ResetDefaultLayout = false;

            // While an asset editor's major tab is in front the level's dockspace is KEPT ALIVE but not shown:
            // its windows are not submitted (the panel loop skips them), and KeepAliveOnly keeps them docked
            // where they were, so the Scene tab brings the level layout back untouched.
            const ImVec2 dockOrigin = ::ImGui::GetCursorScreenPos();
            m_Documents.SetMajorTabArea( glm::vec2( dockOrigin.x, dockOrigin.y ),
                                         glm::vec2( dockSize.x, dockSize.y ) );
            ::ImGui::DockSpace( dockspace_id, dockSize,
                                m_Documents.MajorTabActive() ? m_DockspaceFlags | ImGuiDockNodeFlags_KeepAliveOnly
                                                             : m_DockspaceFlags );
            if ( m_Documents.MajorTabActive() )
                ::ImGui::Dummy( dockSize );

            if ( buildDefaultLayout )
            {
                ::ImGui::DockBuilderRemoveNode( dockspace_id );
                ::ImGui::DockBuilderAddNode( dockspace_id, m_DockspaceFlags | ImGuiDockNodeFlags_DockSpace );
                ::ImGui::DockBuilderSetNodeSize( dockspace_id, ( dockSize.x > 0 && dockSize.y > 0 )
                                                                    ? dockSize
                                                                    : ::ImGui::GetMainViewport()->Size );

                //  ┌───────────┬────────────────────────────┬──────────────┐
                //  │ Scene     │ Scene (viewport) + a tab   │ Details      │
                //  │ Outliner  │ per open asset document    ├──────────────┤
                //  ├───────────┤ (DocumentPlacement)        │ SceneSettings│
                //  │Collections├────────────────────────────┤ / Profiler   │
                //  │           │ Assets / Logs / Documents  │              │
                //  └───────────┴────────────────────────────┴──────────────┘
                //
                // THE CENTRE IS WHOLE. An asset document is a tab beside the level, the way Unreal opens an
                // asset editor: it gets the full work area while it is the active tab. The split-off
                // document column this replaced (layout option B.1) left a Material Editor ~400 px wide.
                ImGuiID center = dockspace_id;
                ImGuiID right  = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Right, 0.20f, nullptr, &center );
                ImGuiID left   = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Left, 0.22f, nullptr, &center );
                ImGuiID bottom = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Down, 0.28f, nullptr, &center );
                m_BottomDockId = bottom; // remembered so the drawer can be collapsed/restored later
                ImGuiID leftBottom = ::ImGui::DockBuilderSplitNode( left, ImGuiDir_Down, 0.40f, nullptr, &left );
                ImGuiID rightBottom =
                     ::ImGui::DockBuilderSplitNode( right, ImGuiDir_Down, 0.50f, nullptr, &right );

                // Panels routed through the central Begin carry an icon (a ### suffix), so dock them by the
                // SAME composed title — otherwise the icon-changed ImGui ID wouldn't match this assignment.
                // Non-panel windows (Profiler / Shader Code) self-Begin with plain names.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Scene###scene" ).c_str(), center );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Scene Outliner" ).c_str(), left );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Collections" ).c_str(), leftBottom );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Details" ).c_str(), right );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "World Settings" ).c_str(), rightBottom );
                ::ImGui::DockBuilderDockWindow( "Profiler", rightBottom );
                // NO LINE FOR "Foliage##FoliagePanel" (FO-UI1): docked here it became a tab behind Scene Settings
                // that entering Foliage mode never showed, in a node ~300 px tall. It floats over the viewport's
                // left edge while Foliage mode is on (FoliagePaintTool::DrawPanel), as UE's mode toolkit sits
                // beside the level.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Assets" ).c_str(), bottom );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Logs" ).c_str(), bottom );
                ::ImGui::DockBuilderDockWindow( "Shader Code", bottom );

                // Contextual tools (IPanel::IsContextual) get a home too, so the one that opens itself
                // lands where its work belongs instead of floating over the scene: timelines along the
                // bottom next to Assets/Logs, authoring palettes on the right beside Details.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Anim Layers" ).c_str(), bottom );
                // No line for "Anim Graph", "Particle Editor", "UI Editor" or "Sequencer": they are
                // documents, and a document does not have a fixed home in the layout — it docks into the
                // document well beside the others (DrawDocuments sets the dock id), which is the whole point
                // of the well existing. A line here would also name a window that no longer exists under
                // that title: a document's ImGui id is "###doc<subject>", so it could never have matched.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Modeling" ).c_str(), left );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Landscape" ).c_str(), left );
                // UE's World Partition editor is a docked tab whose map fills it. The left column is the
                // tallest node that is not the level, so the map gets a near-square canvas beside the Outliner.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "World Partition" ).c_str(), left );

                // The well is the INDEX of open documents, not where they open: a document opens as a tab
                // beside the level viewport (DocumentPlacement), so the well is a tab in the drawer and
                // the centre stays whole for the level and the documents.
                ::ImGui::DockBuilderDockWindow( DocumentHost::WellWindowTitle(), bottom );
                m_Documents.Well().ShowWindow(); // the default layout has the well open

                ::ImGui::DockBuilderFinish( dockspace_id );
            }

            // ── THE FOUR-UP GRID, APPLIED ─────────────────────────────────────────────────────────
            //
            // It splits THE NODE THE MAIN VIEWPORT IS IN, not the whole dockspace: the Outliner, Details
            // and the bottom drawer keep their places, and the grid costs exactly the pixels the single
            // viewport had. That is the difference between "a layout command" and "Reset to Default
            // Layout with four viewports in it", and it is why this is not folded into the block above.
            //
            // A FLOATING main viewport has no node to split (DockId 0); the dockspace is the honest
            // fallback, and it is said out loud because the result then displaces the other panels.
            if ( const auto& grid = m_Workspace.PendingViewportGrid(); !grid.empty() )
            {
                ImGuiID node = 0;
                if ( const ::ImGuiWindow* win = ::ImGui::FindWindowByName( PanelDisplayTitle( grid[0] ).c_str() ) )
                    node = win->DockId;
                if ( node == 0 || ::ImGui::DockBuilderGetNode( node ) == nullptr )
                {
                    LOG_WARN( "[Editor] the main viewport is not docked; the grid takes the whole "
                              "dockspace, so other panels move." );
                    node = dockspace_id;
                }

                // Quarters, in the order BuildViewportGrid filled the list: top-left, top-right,
                // bottom-left, bottom-right. A list shorter than four (the slot budget refused a pane)
                // simply leaves that quarter to its neighbours, which is what DockBuilder does with an
                // empty node.
                ImGuiID topLeft = node;
                ImGuiID topRight =
                     ::ImGui::DockBuilderSplitNode( topLeft, ImGuiDir_Right, 0.5f, nullptr, &topLeft );
                const ImGuiID bottomLeft =
                     ::ImGui::DockBuilderSplitNode( topLeft, ImGuiDir_Down, 0.5f, nullptr, &topLeft );
                const ImGuiID bottomRight =
                     ::ImGui::DockBuilderSplitNode( topRight, ImGuiDir_Down, 0.5f, nullptr, &topRight );

                const ImGuiID quarters[4] = { topLeft, topRight, bottomLeft, bottomRight };
                for ( size_t i = 0; i < grid.size() && i < 4; ++i )
                    ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( grid[i] ).c_str(), quarters[i] );

                ::ImGui::DockBuilderFinish( dockspace_id );
                m_Workspace.ClearPendingViewportGrid();
            }
        }
    }

    void DockLayout::DrawPanels()
    {
        // THE TOOLS. The document loop is DrawDocuments, below, and the two are separate for the reason the
        // whole task exists: a tool passes &GetVisibility() to Begin, which is right for a setting the user
        // keeps, and a document must not — its false would be read as "destroy this window".
        //
        // The cascade this loop used to carry for documents is gone with them: a document is DOCKED into the
        // well now, so there is no floating window to step down-right from the last one.
        for ( const auto& panel : m_Panels )
        {
            if ( !panel->GetVisibility() || m_Documents.MajorTabActive() )
            {
                continue;
            }

            namespace ImGui = ::ImGui;
            // One padding rule for the whole editor, declared by the panel (the viewport asks for zero).
            // The panel states it as a glm::vec2 -- IPanel.hpp must not name the toolkit -- and this is
            // the line that draws, so this is where it becomes an ImVec2.
            const glm::vec2 padding = panel->GetWindowPadding();
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( padding.x, padding.y ) );

            // First-ever open: give the panel its preferred size, centered on the main viewport —
            // floating tools no longer pop up as tiny windows in a corner. imgui.ini keeps the
            // user's layout afterwards (FirstUseEver never fights it).
            if ( const glm::vec2 defSize = panel->GetDefaultSize(); defSize.x > 0.0f && defSize.y > 0.0f )
            {
                ImGui::SetNextWindowSize( ImVec2( defSize.x, defSize.y ), ImGuiCond_FirstUseEver );
                ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                                         ImVec2( 0.5f, 0.5f ) );
            }

            // p_open: the title-bar X closes the panel and stays in sync with the View menu. The display
            // title carries an icon but keeps the ImGui ID == GetName() (see PanelDisplayTitle).
            // A panel that just auto-opened is brought to the front of its dock, otherwise it would
            // appear as a background tab nobody notices.
            if ( !m_FocusPanel.empty() && panel->GetName() == m_FocusPanel )
            {
                ImGui::SetNextWindowFocus();
                m_FocusPanel.clear();
            }

            // Maximize / restore (palette "Panel" group): the dock node is read from the window as it stands,
            // so a panel the user re-docked by hand is simply not maximized any more.
            {
                const ImGuiWindow* window =
                     ImGui::FindWindowByName( PanelDisplayTitle( panel->GetName() ).c_str() );
                const std::uint32_t dockId = window != nullptr ? window->DockId : 0;
                const auto directive       = m_PanelMaximize.Before( PanelShownName( panel->GetName() ), dockId );
                switch ( directive.Kind )
                {
                    case PanelMaximize::Step::Undock:
                    {
                        const ImGuiViewport* viewport = ImGui::GetMainViewport();
                        ImGui::SetNextWindowDockID( 0, ImGuiCond_Always );
                        ImGui::SetNextWindowViewport( viewport->ID );
                        ImGui::SetNextWindowPos( viewport->WorkPos, ImGuiCond_Always );
                        ImGui::SetNextWindowSize( viewport->WorkSize, ImGuiCond_Always );
                        ImGui::SetNextWindowFocus();
                        break;
                    }
                    case PanelMaximize::Step::Redock:
                        ImGui::SetNextWindowDockID( directive.DockId, ImGuiCond_Always );
                        ImGui::SetNextWindowFocus();
                        break;
                    case PanelMaximize::Step::None:
                        break;
                }
            }

            ImGui::Begin( PanelDisplayTitle( panel->GetName() ).c_str(), &panel->GetVisibility() );
            ImGui::PopStyleVar(); // right after Begin: the window kept it, child windows must not inherit
            {
                DESERT_PROFILE_SCOPE_DYNAMIC( panel->GetName().c_str() );
                panel->OnUIRender();
            }
            panel->TrackWindowInteraction();
            ImGui::End();
        }
    }

    void DockLayout::EndHost()
    {
        ::ImGui::End(); // End dockspace
    }

    void DockLayout::DrawRecoveryPopup()
    {
        namespace ImGui = ::ImGui;

        if ( !m_ShowRecoveryPrompt )
            return;

        constexpr const char* kId = "Recover unsaved work?##recovery";
        ImGui::OpenPopup( kId );

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "The previous session ended unexpectedly." );
            ImGui::Spacing();
            ImGui::Text( "Reopen the latest autosave?\n%s", m_RecoveryAutosave.filename().string().c_str() );
            ImGui::Spacing();
            ImGui::TextDisabled( "It opens as an unsaved scene — Save to keep it." );
            ImGui::Separator();

            if ( ImGui::Button( "Reopen autosave", ImVec2( 150.0f, 0.0f ) ) )
            {
                m_SceneFiles.RequestLoad( m_RecoveryAutosave );
                m_ShowRecoveryPrompt = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Ignore", ImVec2( 100.0f, 0.0f ) ) )
            {
                m_ShowRecoveryPrompt = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void DockLayout::DrawLayoutSavePopup()
    {
        namespace ImGui = ::ImGui;

        if ( !m_ShowSaveLayoutPopup )
            return;

        constexpr const char* kId = "Save Layout##saveLayout";
        ImGui::OpenPopup( kId );

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "Layout name:" );
            ImGui::SetNextItemWidth( 260.0f );
            const bool submit = ImGui::InputText( "##layoutName", m_LayoutNameBuf, sizeof( m_LayoutNameBuf ),
                                                  ImGuiInputTextFlags_EnterReturnsTrue );

            const bool valid = !LayoutManager::Sanitize( m_LayoutNameBuf ).empty();
            ImGui::BeginDisabled( !valid );
            if ( ( ImGui::Button( "Save", ImVec2( 110.0f, 0.0f ) ) || submit ) && valid )
            {
                if ( !LayoutManager::Save( m_LayoutNameBuf ) )
                    Editor::ToastManager::Push( "The layout was not saved (see the log)",
                                                Editor::ToastLevel::Error );
                m_ShowSaveLayoutPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            {
                m_ShowSaveLayoutPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Collapse/restore the bottom drawer (the dock node holding Assets / Logs / Shader Code).
    //
    // ImGui has no "collapse a dock node" call — a docked window trades its collapse arrow for a tab.
    // So collapsing is done by SIZE: the node is squeezed down to its tab bar and restored to the height
    // it had before. That keeps the tabs on screen, which is the whole point of collapsing rather than
    // closing, and it leaves the user's own resize intact because the height is re-read at collapse time.
    void DockLayout::DrawBottomDrawerToggle()
    {
        namespace ImGui = ::ImGui;

        // Resolve the drawer node from the Assets window's ACTUAL dock node, not from the id captured while
        // building the default layout: that branch only runs for a fresh layout, so with a restored
        // imgui.ini the id stayed 0 and this control was permanently dead.
        ImGuiDockNode* node = m_BottomDockId ? ImGui::DockBuilderGetNode( m_BottomDockId ) : nullptr;
        if ( !node )
        {
            if ( ImGuiWindow* assets = ImGui::FindWindowByName( PanelDisplayTitle( "Assets" ).c_str() );
                 assets && assets->DockNode )
            {
                node           = assets->DockNode;
                m_BottomDockId = node->ID;
            }
        }
        if ( !node )
        {
            ImGui::TextDisabled( ICON_MDI_CHEVRON_DOWN );
            return;
        }

        // Tab bar height + the node's own padding — what "collapsed" means for this node.
        const float collapsedHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

        const char* icon = m_BottomCollapsed ? ICON_MDI_CHEVRON_UP : ICON_MDI_CHEVRON_DOWN;
        if ( ImGui::SmallButton( icon ) )
        {
            m_BottomCollapsed = !m_BottomCollapsed;
            if ( m_BottomCollapsed )
            {
                // Remember the CURRENT height, not the default: the user may have dragged the splitter.
                m_BottomHeight = node->Size.y;
                ImGui::DockBuilderSetNodeSize( m_BottomDockId, ImVec2( node->Size.x, collapsedHeight ) );
            }
            else
            {
                const float restore = m_BottomHeight > collapsedHeight
                                           ? m_BottomHeight
                                           : ImGui::GetMainViewport()->Size.y * 0.28f; // the layout default
                ImGui::DockBuilderSetNodeSize( m_BottomDockId, ImVec2( node->Size.x, restore ) );
            }
            ImGui::DockBuilderFinish( m_BottomDockId );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( m_BottomCollapsed ? "Expand the bottom drawer (Assets / Logs)"
                                                 : "Collapse the bottom drawer (Assets / Logs)" );
    }

    void DockLayout::AppendPanelCommands( std::vector<PaletteCommand>& commands )
    {
        // Panels — jump to / reveal any tool window. TOOLS ONLY, and by construction rather than by a
        // filter: m_Panels is a PanelRegistry, which cannot hold a document. Before the split this loop
        // offered "Open M_Crate_Painted###assetdoc..." as a panel, and running it set a visibility flag that
        // the close pass then read as "the user dismissed this window".
        for ( const auto& panel : m_Panels )
        {
            IPanel*     p    = panel.get();
            std::string name = p->GetName();
            if ( const auto hash = name.find( "##" ); hash != std::string::npos )
                name.erase( hash ); // drop the "###id" ImGui suffix for display
            // Through PanelRequests, the one "show that panel" wire: it also brings the panel's tab
            // forward, which setting visibility alone never did for a panel already docked behind another.
            commands.push_back( { "Panel", "Open " + name, [p]
                                  {
                                      Core::PanelRequests::Open( p->GetName() );
                                      return PaletteCommandDone();
                                  } } );
        }
    }

    void DockLayout::AppendMaximizeCommands( std::vector<PaletteCommand>& commands )
    {
        // Maximize any panel that sits in a dock now; restore the maximized one.
        std::vector<std::string> docked;
        for ( const auto& panel : m_Panels )
        {
            if ( !panel->GetVisibility() )
                continue;
            const ImGuiWindow* window = ::ImGui::FindWindowByName( PanelDisplayTitle( panel->GetName() ).c_str() );
            if ( window != nullptr && window->DockId != 0 )
                docked.push_back( PanelShownName( panel->GetName() ) );
        }
        for ( PaletteCommand& command : PanelMaximizePaletteCommands( m_PanelMaximize, docked ) )
            commands.push_back( std::move( command ) );
    }

    void DockLayout::AppendWindowCommands( std::vector<PaletteCommand>& commands, Engine::Application* frameless )
    {
        // THE WINDOW'S OWN COMMANDS, offered only when this editor owns its frame — with a system frame
        // they would be a second set of buttons for three things the OS already does, and the palette would
        // be offering to press a button that is right there.
        //
        // They are here for the reason Г14 put the palette itself on the channel: a capability reachable
        // only by a mouse does not exist for an unattended run. The title bar's buttons and its
        // double-click call exactly these two window methods, so a client that cannot click can still put
        // the window through maximize and restore and photograph what came out — which is the ONLY way the
        // maximize path in this build has been executed at all, the gesture itself being unsynthesisable
        // on this machine.
        if ( frameless != nullptr )
        {
            const auto& window = frameless->GetWindow();
            commands.push_back( { "Window", "Maximize", [window]
                                  {
                                      window->Maximize();
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Window", "Restore", [window]
                                  {
                                      window->Restore();
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Window", "Minimize", [window]
                                  {
                                      window->Minimize();
                                      return PaletteCommandDone();
                                  } } );
        }
    }

} // namespace Desert::Editor

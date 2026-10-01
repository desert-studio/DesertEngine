#include "Editor/LevelEditor/MainMenu.hpp"

#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/Commands/SceneCommands.hpp"
#include "Editor/Core/DocumentWell.hpp"
#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/Core/LayoutManager.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/Core/ProjectContext.hpp"
#include "Editor/Core/Selection/SelectionManager.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/PreferencesWindow.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/WindowTitles.hpp"
#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <Engine/Core/Scene.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <span>
#include <unordered_set>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        // THE MENU BAR'S OWN MENUS, named once. Read by DrawMenus, which opens whichever one is held, and
        // by AppendMenuCommands, which offers exactly these as commands. Two readers of one list, so the
        // palette cannot offer a menu the bar does not draw — the shape a hand-copied second list always ends
        // up in.
        constexpr const char* kMenuBarMenus[] = { "File",   "Edit",     "View", "Window",
                                                  "Scenes", "Graphics", "About" };
    } // namespace

    MainMenu::MainMenu( SceneWorkspace& workspace, SceneFiles& sceneFiles, DocumentHost& documents,
                        PanelRegistry& panels, PreferencesWindow& preferences, bool& showProfiler,
                        MainMenuActions actions )
         : m_Workspace( workspace ), m_SceneFiles( sceneFiles ), m_Documents( documents ), m_Panels( panels ),
           m_Preferences( preferences ), m_ShowProfiler( showProfiler ), m_Actions( std::move( actions ) )
    {
    }

    void MainMenu::DrawMenus()
    {
        namespace ImGui = ::ImGui;

        // THE MENU HELD OPEN, if the control channel asked for one. `--open-menu <name>` stood here and
        // it could hold a menu open for the whole run and never let go, because a flag has no later
        // moment at which to be told otherwise. "Menu" / "Open the View menu" is now an ordinary palette
        // entry, so a session can photograph a menu and then close it and carry on.
        //
        // OpenPopup here and BeginMenu below derive the same id from the same label in the same window
        // (BeginMenu: window->GetID(label); OpenPopup: CurrentWindow->GetID(str_id)), which is what makes
        // this the menu's own opening rather than a second popup wearing its name. Re-issued every frame
        // because a menu closes as soon as focus leaves it and a shot may land on any frame.
        //
        // The name was validated against kMenuBarMenus when the command was built, so there is no unknown
        // name to reject here: the palette cannot offer one.
        if ( !m_HeldOpenMenu.empty() )
            ImGui::OpenPopup( m_HeldOpenMenu.c_str() );

        DrawFileMenu();
        DrawEditMenu();
        DrawViewMenu();
        DrawWindowMenu();
        m_SceneFiles.DrawScenesMenu();
        DrawGraphicsMenu();
        DrawAboutMenu();
    }

    void MainMenu::AppendMenuCommands( std::vector<PaletteCommand>& commands )
    {
        // THE MENU BAR. `--open-menu` is gone and this is where its capability went: a menu can be opened,
        // photographed and closed again, as many times as a session likes, instead of being pinned open
        // for a whole run by a flag with no way to say "now let go".
        for ( const char* menu : kMenuBarMenus )
        {
            const std::string name = menu;
            commands.push_back( { "Menu", "Open the " + name + " menu", [this, name]
                                  {
                                      m_HeldOpenMenu = name;
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Menu", "Close the open menu", [this]
                              {
                                  m_HeldOpenMenu.clear();
                                  return PaletteCommandDone();
                              } } );
    }

    void MainMenu::AppendUndoRedoCommands( std::vector<PaletteCommand>& commands )
    {
        // UNDO AND REDO ALREADY ANSWERED "was there anything to undo" and the answer went nowhere.
        // Nothing to undo is not a failure of the editor, but it IS the difference between a script that
        // walked the history back one step and a script that believes it did.
        commands.push_back( { "Action", "Undo", [] {
                                 return PaletteCommandOutcome( CommandHistory::Get().Undo(),
                                                               "there was nothing left to undo." );
                             } } );
        commands.push_back( { "Action", "Redo", [] {
                                 return PaletteCommandOutcome( CommandHistory::Get().Redo(),
                                                               "there was nothing to redo." );
                             } } );
    }

    void MainMenu::DrawFileMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "File" ) )
        {
            return;
        }

        // The editor is bound to ONE project per run (all content paths are remapped at startup).
        // Switching projects = relaunching through the Project Hub, so the menu only SHOWS the project.
        ImGui::MenuItem(
             ( std::string( ICON_MDI_PACKAGE_VARIANT " " ) + Editor::ProjectContext::Current().Name ).c_str(),
             nullptr, false, false );
        ImGui::TextDisabled( "  switch projects via the Project Hub" );
        ImGui::Separator();

        if ( ImGui::MenuItem( "Open File" ) )
        {
        }
        ImGui::Separator();

        if ( ImGui::MenuItem( "New Scene", "CTRL+N" ) )
        {
            m_SceneFiles.RequestNew();
        }
        if ( ImGui::MenuItem( "Save Scene", "CTRL+S" ) )
        {
            m_SceneFiles.RequestSave();
        }
        if ( ImGui::MenuItem( "Reload Scene", "CTRL+R" ) )
        {
        }

        m_SceneFiles.DrawOpenSceneMenuItem();
        DrawStyleSubmenu();

        ImGui::Separator();

        if ( ImGui::MenuItem( "Rebuild Cooked Assets" ) )
        {
            m_Actions.RebuildCookedAssets();
        }

        ImGui::Separator();

        // IT HAD AN EMPTY BODY. Found while У9 was giving the close button one: File ▸ Exit has been a
        // menu entry that does nothing since it was written, and with the system frame gone it would have
        // been the only way out of the editor other than killing the process. Same ordered close as the
        // title bar's x and the control channel's `quit`.
        if ( ImGui::MenuItem( "Exit" ) )
            m_Actions.RequestExit();

        ImGui::EndMenu();
    }

    void MainMenu::DrawStyleSubmenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Style" ) )
        {
            return;
        }

        if ( ImGui::MenuItem( "Dark" ) )
        {
            ThemeManager::SetDarkTheme();
        }

        if ( ImGui::MenuItem( "Black" ) )
        {
            ThemeManager::SetBlackTheme();
        }

        ImGui::EndMenu();
    }

    void MainMenu::DrawEditMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Edit" ) )
        {
            return;
        }

        const bool editMode     = m_Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Edit;
        const bool hasSelection = Core::SelectionManager::Count() > 0;

        if ( ImGui::MenuItem( "Undo", "Ctrl+Z", false, editMode ) )
            CommandHistory::Get().Undo();
        if ( ImGui::MenuItem( "Redo", "Ctrl+Shift+Z", false, editMode ) )
            CommandHistory::Get().Redo();

        ImGui::Separator();

        if ( ImGui::MenuItem( "Copy", "Ctrl+C", false, editMode && hasSelection ) )
            Commands::CopySelectionToClipboard( Core::SelectionManager::GetSelection() );
        if ( ImGui::MenuItem( "Paste", "Ctrl+V", false, editMode && Commands::ClipboardHasContent() ) )
        {
            if ( auto pasted = Commands::PasteClipboard(); !pasted.empty() )
                Core::SelectionManager::SetSelection( std::move( pasted ) );
        }
        if ( ImGui::MenuItem( "Duplicate", "Ctrl+D", false, editMode && hasSelection ) )
        {
            if ( auto dups = Commands::DuplicateEntities( Core::SelectionManager::GetSelection() ); !dups.empty() )
                Core::SelectionManager::SetSelection( std::move( dups ) );
        }
        if ( ImGui::MenuItem( "Delete", "Del", false, editMode && hasSelection ) )
            Commands::DeleteEntities( Core::SelectionManager::GetSelection() );

        ImGui::Separator();
        if ( ImGui::MenuItem( "Preferences..." ) )
            m_Preferences.Open();

        ImGui::EndMenu();
    }

    void MainMenu::DrawViewMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "View" ) )
        {
            return;
        }

        // GROUPED BY WHAT THE ENTRY IS FOR. A flat alphabet-of-whatever-was-constructed-first list is a list
        // nobody reads; the ones that answer "where do I look at the level / the content / the output" stay
        // at the top level and the ones that belong to a particular job move behind the job's own submenu.
        // Nothing is deleted and nothing becomes unreachable — see the leftover section at the end, which is
        // empty when every panel is placed.
        //
        // THE COUNT USED TO BE WRITTEN OUT HERE ("twenty-one tools, twelve entries") AND IT WAS WRONG. Tools
        // are added to and taken out of m_Panels by every task that promotes one to a document, and a scene
        // view joins the same registry at RUNTIME (SceneWorkspace::AddSceneView) — so no number typed in this
        // comment can be right for a whole session, let alone across a release. The leftover section below
        // is the census that cannot drift, because it is computed from the registry it describes.
        //
        // AND NO DOCUMENTS. Not because this loop skips them: because m_Panels is a PanelRegistry and
        // cannot hold one. That is the whole task. Open documents are in Window -> Documents, where the
        // control is a radio and the close is an x, neither of which can be mistaken for "hide".
        // THE GROUPING IS DATA, NOT CONTROL FLOW, and that is a correction rather than a preference: a
        // submenu's body only runs while it is OPEN, so marking a panel "placed" from inside one reported
        // every panel behind a closed submenu as ungrouped. Measured — the first capture of this menu showed
        // ten panels under "NOT YET GROUPED" that are grouped. The census has to be readable without opening
        // anything, so it is stated once here and the drawing below refers to it.
        static constexpr const char* kLevelGroup[]    = { "Scene Outliner", "Collections",      "Details",
                                                          "World Settings", "Scene Validation", "World Partition" };
        static constexpr const char* kContentGroup[]  = { "Assets", "Asset References", "Shader Library" };
        static constexpr const char* kOutputGroup[]   = { "Logs", "Lua Console", "History" };
        static constexpr const char* kViewportGroup[] = { "Scene###scene" };
        // "Anim Graph", "Particle Editor", "UI Editor" and "Sequencer" are gone from these lists because
        // they are gone from the registry this menu loops over — a name left here would draw a group entry
        // for a panel that does not exist. They are opened from the component that holds them, in Details.
        // "Node Graph" is gone from here with the panel: the shader graph is a DOCUMENT, opened from the
        // `.dgraph` in the asset browser or from the palette's Open group, and the whole `Graph Editors`
        // submenu went with it rather than being left to draw an empty body.
        static constexpr const char* kSequencerGroup[] = { "Anim Layers" };
        // Localization sits with the tools rather than with the level: it is about the PROJECT's strings,
        // not about the scene that happens to be open, and it keeps answering after every scene change.
        static constexpr const char* kToolGroup[] = { "Modeling", "Model from Photos", "Build Settings",
                                                      "Scalability", "Localization" };

        std::unordered_set<std::string> placed;
        for ( const auto& group :
              { std::span<const char* const>( kLevelGroup ), std::span<const char* const>( kContentGroup ),
                std::span<const char* const>( kOutputGroup ), std::span<const char* const>( kViewportGroup ),
                std::span<const char* const>( kSequencerGroup ), std::span<const char* const>( kToolGroup ) } )
            for ( const char* name : group )
                placed.insert( name );

        auto panelItem = [&]( const char* name )
        {
            for ( auto& panel : m_Panels )
            {
                if ( panel->GetName() != name )
                    continue;

                // Same icon + stable ID as the panel title (the ###id keeps each menu entry unique/stable).
                const bool wasVisible = panel->GetVisibility();
                if ( ImGui::MenuItem( PanelDisplayTitle( panel->GetName() ).c_str(), "", &panel->GetVisibility(),
                                      true ) )
                {
                    // Ticking a contextual panel pins it open; unticking releases it back to the context.
                    if ( panel->IsContextual() )
                        panel->Pinned() = !wasVisible;
                }
                if ( panel->IsContextual() && ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Opens itself when its context appears. Ticking it keeps it open "
                                       "even when it does not apply." );
                return;
            }
        };

        auto group = [&]( std::span<const char* const> names )
        {
            for ( const char* name : names )
                panelItem( name );
        };

        ImGui::TextDisabled( "THE LEVEL" );
        group( kLevelGroup );

        ImGui::Separator();
        ImGui::TextDisabled( "CONTENT" );
        group( kContentGroup );

        ImGui::Separator();
        ImGui::TextDisabled( "OUTPUT" );
        group( kOutputGroup );
        // The Profiler is a window this layer draws itself rather than an IPanel, so it is a bool and not a
        // registry entry — it belongs in the group all the same, because the user is choosing between it and
        // the Logs beside it, not between two implementations.
        ImGui::MenuItem( ICON_MDI_CHART_BAR "  Profiler", "", &m_ShowProfiler, true );

        ImGui::Separator();

        // The nine that moved. Each is behind the job it belongs to rather than in a flat list beside
        // "Details" — a viewport is not a panel you tick, and a timeline is somewhere you go to author a
        // clip.
        if ( ImGui::BeginMenu( ICON_MDI_MONITOR "  Viewports" ) )
        {
            group( kViewportGroup );
            ImGui::Separator();
            // Multi-scene editing: a second, independent scene in its own live viewport (own SceneRenderer)
            // so a UI scene and the game scene can be worked on side by side. Focus a viewport to make its
            // scene active — the Outliner / Details / gizmo follow it.
            if ( ImGui::MenuItem( ICON_MDI_PLUS_BOX_MULTIPLE " New Scene View" ) )
                m_Workspace.RequestAddSceneView(); // serviced in OnUpdate (allocates GPU resources)
            // A SECOND ANGLE, and FOUR of them. Both are about the ACTIVE scene, not a second one, which
            // is what the item above opens — the menu says so in the tooltips because the two read alike.
            if ( ImGui::MenuItem( ICON_MDI_BORDER_ALL " New Viewport (same scene)" ) )
                m_Workspace.RequestAddSceneViewport();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Another camera on the SAME world: same entities, same edits." );
            if ( ImGui::MenuItem( ICON_MDI_GRID " Four-Up Viewports" ) )
                m_Workspace.RequestViewportGrid();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Perspective, Top, Front and Right on the active scene, docked in a\n"
                                   "2x2 grid. The panes are ordinary windows: re-arrange them and save\n"
                                   "the result under View > Layouts." );
            if ( !m_Workspace.Documents().empty() )
                ImGui::TextDisabled( "%d scene view(s) open + main",
                                     static_cast<int>( m_Workspace.Documents().size() ) );
            // Closing from here does exactly what the window's x does — clear the VIEWPORT PANEL's
            // visibility — rather than tearing the scene down inside the ImGui pass. A scene view is a tool
            // panel bound to a scene, so visibility genuinely is its close signal; a document is the case
            // where that stopped being true, which is why documents have their own path.
            for ( const auto& doc : m_Workspace.Documents() )
            {
                const std::string item = std::string( ICON_MDI_CLOSE " Close " ) + doc->Name;
                if ( ImGui::MenuItem( item.c_str() ) && doc->Viewport )
                    doc->Viewport->GetVisibility() = false;
            }
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( ICON_MDI_CHART_TIMELINE "  Sequencer" ) )
        {
            // Both together: a clip is authored in the timeline and its layers, and two independent ticks
            // for one place you go was two decisions where there is one.
            group( kSequencerGroup );
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( ICON_MDI_HAMMER_WRENCH "  Tools" ) )
        {
            group( kToolGroup );
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( ICON_MDI_EYE "  Show" ) )
        {
            if ( ImGui::MenuItem( "Perf HUD", "", &EditorPreferences::Get().ShowPerfHud, true ) )
                EditorPreferences::Save(); // persist the toggle like the rest of the user prefs
            ImGui::EndMenu();
        }

        // ANYTHING THE GROUPS ABOVE DID NOT NAME. This is empty today and is not a placeholder: a panel
        // added later and forgotten here would otherwise have no menu entry at all, which is the same
        // "you cannot get it back" the documents had. It is visible precisely so that it gets fixed.
        {
            bool anyLeftover = false;
            for ( auto& panel : m_Panels )
            {
                if ( placed.count( panel->GetName() ) != 0 )
                    continue;
                if ( !anyLeftover )
                {
                    ImGui::Separator();
                    ImGui::TextDisabled( "NOT YET GROUPED" );
                    anyLeftover = true;
                }
                panelItem( panel->GetName().c_str() );
            }
        }

        ImGui::Separator();
        if ( ImGui::BeginMenu( "Layouts" ) )
        {
            for ( const auto& name : LayoutManager::List() )
            {
                if ( ImGui::MenuItem( name.c_str() ) )
                    LayoutManager::Load( name );
                if ( ImGui::IsItemHovered() && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) )
                    LayoutManager::Delete( name ); // right-click removes it
            }
            ImGui::Separator();
            if ( ImGui::MenuItem( "Save Current Layout..." ) )
                m_Actions.SaveLayoutAs();
            if ( ImGui::MenuItem( "Reset to Default Layout" ) )
                m_Actions.ResetLayout();
            ImGui::EndMenu();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Named docking layouts. Right-click a layout to delete it." );

        ImGui::EndMenu();
    }

    void MainMenu::DrawWindowMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Window" ) )
            return;

        // The well's own window, the way back after its x. Unticking it here is the same close.
        ImGui::MenuItem( ICON_MDI_FILE_DOCUMENT_MULTIPLE_OUTLINE "  Documents", nullptr,
                         &m_Documents.Well().WindowOpenFlag() );
        ImGui::Separator();

        // A SECOND MENU, BECAUSE THESE ARE A SECOND KIND OF THING. The View menu ticks tools on and off;
        // this one lists what is open and lets you go to it or close it. Putting documents back among the
        // ticks is the defect, not the layout.
        if ( m_Documents.Documents().Empty() )
        {
            ImGui::TextDisabled( "No document open" );
            ImGui::TextDisabled( "Double-click an asset in the Content Browser." );
        }
        else
        {
            ImGui::TextDisabled( "OPEN DOCUMENTS \xe2\x80\x94 %zu", m_Documents.Documents().Count() );

            // The x column is placed against the WIDEST row, measured, not against the popup's content
            // region: a menu auto-sizes to its widest item, so asking the region where the right edge is
            // gives an answer that depends on the answer. (Measured — the first capture of this menu had no
            // x on any row, because every one of them was placed past the edge it was helping to define.)
            float widestRow = 0.0f;
            for ( const auto& document : m_Documents.Documents() )
            {
                const std::string measured = std::string( ICON_MDI_RADIOBOX_MARKED ) + "  " +
                                             m_Documents.DocumentIcon( document->Subject() ) +
                                             std::string( "  " ) + DocumentDisplayName( document->GetName() );
                widestRow = std::max( widestRow, ImGui::CalcTextSize( measured.c_str() ).x );
            }

            std::vector<SubjectId> closeRequests;
            for ( const SubjectId& subject : m_Documents.Well().MostRecentOrder() )
            {
                const ISubjectDocument* document = m_Documents.Documents().Find( subject );
                if ( !document )
                    continue;

                ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( subject ) & 0x7fffffff ) );

                // A RADIO, NOT A CHECKBOX, and the difference is the whole argument of this task written
                // in one glyph. A tick says "shown / hidden" and invites the user to untick it — which is
                // exactly what used to destroy the document. A radio says "this is the one you are in",
                // which is true, is the only thing picking a row can mean, and offers no way to un-pick.
                const bool        active = ( subject == m_Documents.FocusedDocument() );
                const std::string label =
                     std::string( active ? ICON_MDI_RADIOBOX_MARKED : ICON_MDI_RADIOBOX_BLANK ) + "  " +
                     m_Documents.DocumentIcon( document->Subject() ) + std::string( "  " ) +
                     DocumentDisplayName( document->GetName() );

                if ( ImGui::MenuItem( label.c_str() ) )
                    m_Documents.FocusDocument( subject );

                ImGui::SameLine( ImGui::GetCursorPosX() + widestRow + 24.0f );
                if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
                    closeRequests.push_back( subject );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Close this document. It is destroyed, and its renderer slot (if it "
                                       "holds one) is returned." );

                ImGui::PopID();
            }

            ImGui::Separator();
            if ( ImGui::MenuItem( ICON_MDI_CLOSE_BOX_OUTLINE "  Close All Documents" ) )
                m_Documents.RequestCloseAllDocuments();

            for ( const SubjectId& subject : closeRequests )
                m_Documents.AskDocumentClose( subject, "closed from Window \xe2\x96\xb8 Documents" );
        }

        // NO "SAVE ALL" HERE, AND ITS ABSENCE IS DELIBERATE.
        //
        // The mock draws one. It cannot be built honestly yet: ISubjectDocument declares no Save() and no
        // IsDirty(), the editor's single dirty flag belongs to the SCENE (a CommandHistory revision), and a
        // material document writes straight into the in-memory asset as a slider moves. "Save All" would
        // therefore have to mean "rewrite every open document's file whether or not it changed", it could
        // not report how many of them needed it, and it would touch mtimes the asset hot-reload watches.
        // A per-document dirty flag with a working copy behind it is the next task; the item waits for it.

        ImGui::EndMenu();
    }

    void MainMenu::DrawGraphicsMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Graphics" ) )
        {
            return;
        }

        ImGui::EndMenu();
    }

    void MainMenu::DrawAboutMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "About" ) )
        {
            return;
        }

        ImGui::TextUnformatted( "Desert Engine Editor" );
        ImGui::EndMenu();
    }

} // namespace Desert::Editor

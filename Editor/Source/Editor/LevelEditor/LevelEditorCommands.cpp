#include "Editor/LevelEditor/LevelEditorCommands.hpp"

#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/Commands/SceneCommands.hpp"
#include "Editor/Core/DebugCommands.hpp"
#include "Editor/Core/DetailsNavigation.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/Core/Rigging/HumanoidCommands.hpp"
#include "Editor/Core/SaveShortcut.hpp"
#include "Editor/Core/Selection/SelectionManager.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/LevelEditor/AssetCompiling.hpp"
#include "Editor/LevelEditor/DockLayout.hpp"
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/MainMenu.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/ViewportCommands.hpp"
#include "Editor/Panels/Build/BuildCommands.hpp"
#include "Editor/Panels/Clouds/CloudCommands.hpp"
#include "Editor/Panels/Foliage/FoliageCommands.hpp"
#include "Editor/Panels/Landscape/LandscapeCommands.hpp"
#include "Editor/Panels/Localization/LanguageCommands.hpp"
#include "Editor/Panels/Modeling/ModelingCommands.hpp"
#include "Editor/Panels/Scalability/AntiAliasingPaletteCommands.hpp"
#include "Editor/Panels/SceneHierarchy/SceneHierarchyPanel.hpp"
#include "Editor/Panels/UI/UICommands.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"
#include "Editor/Widgets/WindowChrome.hpp"
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <ImGui/imgui.h>

#include <functional>
#include <string>
#include <utility>

namespace Desert::Editor
{
    LevelEditorCommands::LevelEditorCommands( Modules modules )
         : m_Modules( std::move( modules ) ),
           m_EntityCommands( m_Modules.Workspace.ActiveScene(), m_Modules.Documents.SubjectEditors(),
                             [this] { return m_Modules.Workspace.ActiveEditorCamera(); } ),
           m_AssetCommands(
                m_Modules.FileExplorer, m_Modules.WorldPartition, m_Modules.Workspace.ActiveScene(),
                m_Modules.AssetsSlot, m_AssetFiles, [this] { return m_Modules.Workspace.ActiveEditorCamera(); },
                m_Modules.ShowFolder )
    {
    }

    void LevelEditorCommands::RegisterProviders()
    {
        // THE PALETTE'S PROVIDERS, in palette order (CommandRegistry.hpp). Each subject-owned provider reads the
        // main-scene / asset-manager SLOT when an entry runs, so re-pointing the slot needs no re-registration.
        // The order of these calls IS the palette's order of groups (and the control channel's list).
        SceneWorkspace& workspace = m_Modules.Workspace;
        DocumentHost&   documents = m_Modules.Documents;
        DockLayout&     dock      = m_Modules.Dock;

        using Out = std::vector<PaletteCommand>;
        m_Commands.OnBuildBegin( [this] { m_AssetFiles.Take(); } );
        m_Commands.Register( "Panels", [&dock]( Out& out ) { dock.AppendPanelCommands( out ); } );
        m_Commands.Register( "Debug (crash)", []( Out& out ) { AppendCrashCommand( out ); } );
        m_Commands.Register( "Assets (selection)",
                             [this]( Out& out ) { m_AssetCommands.AppendSelectionCommands( out ); } );
        m_Commands.Register( "Panels (maximize)", [&dock]( Out& out ) { dock.AppendMaximizeCommands( out ); } );
        // Details: scroll to a field / open an asset picker, as the last Details frame drew them (CTL2).
        m_Commands.Register( "Details",
                             []( Out& out )
                             {
                                 for ( PaletteCommand& command : DetailsPaletteCommands( GetDetailsNavigation() ) )
                                     out.push_back( std::move( command ) );
                             } );
        // Anti-aliasing method (AA1): the Scalability panel's choice, reachable from the control channel.
        m_Commands.Register( "Anti-aliasing",
                             []( Out& out )
                             {
                                 for ( PaletteCommand& command : AntiAliasingPaletteCommands(
                                            Common::Scalability::QualityState::Catalog() ) )
                                 {
                                     out.push_back( std::move( command ) );
                                 }
                             } );
        m_Commands.Register( "Clouds", []( Out& out ) { AppendCloudCommands( out ); } );
        m_Commands.Register( "Language", []( Out& out ) { AppendLanguageCommands( out ); } );
        m_Commands.Register( "Documents", [&documents]( Out& out ) { documents.AppendDocumentCommands( out ); } );
        m_Commands.Register( "Entity", [this]( Out& out ) { m_EntityCommands.Append( out ); } );
        m_Commands.Register( "Modeling (Mesh To Collision)", [&workspace]( Out& out )
                             { AppendMeshToCollisionCommands( out, workspace.ActiveScene() ); } );
        m_Commands.Register( "Entity (collapse)", []( Out& out ) { EntityCommands::AppendCollapse( out ); } );
        m_Commands.Register( "Menu", [this]( Out& out ) { m_Modules.Menu.AppendMenuCommands( out ); } );
        m_Commands.Register( "Level viewport", []( Out& out ) { AppendViewportCommands( out ); } );
        m_Commands.Register( "Modeling (Select Elements)",
                             []( Out& out ) { AppendSelectElementsCommand( out ); } );
        m_Commands.Register( "Landscape", [&workspace]( Out& out )
                             { AppendLandscapeCommands( out, workspace.ActiveScene() ); } );
        m_Commands.Register( "Modeling (Create Shape)", []( Out& out ) { AppendCreateShapeCommands( out ); } );
        m_Commands.Register( "Humanoid", [&workspace]( Out& out )
                             { AppendHumanoidCommands( out, workspace.ActiveScene() ); } );
        m_Commands.Register( "Scene (add shape)", [this]( Out& out ) { AppendAddShapeCommands( out ); } );
        m_Commands.Register( "Modeling", [&workspace]( Out& out )
                             { AppendModelingCommands( out, workspace.ActiveScene() ); } );
        m_Commands.Register( "UI",
                             [&workspace]( Out& out ) { AppendUICommands( out, workspace.ActiveScene() ); } );
        m_Commands.Register( "Palette", [this]( Out& out ) { AppendPaletteDoorCommand( out ); } );
        m_Commands.Register( "Assets (import)",
                             [this]( Out& out ) { m_AssetCommands.AppendImportCommands( out ); } );
        m_Commands.Register( "Foliage",
                             [this, &workspace]( Out& out ) {
                                 AppendFoliageCommands( out, workspace.ActiveScene(), m_Modules.AssetsSlot,
                                                        m_AssetFiles.Files() );
                             } );
        m_Commands.Register( "Open", [this, &documents]( Out& out )
                             { documents.AppendOpenCommands( out, m_AssetFiles.Files() ); } );
        m_Commands.Register( "Assets (folders)",
                             [this]( Out& out ) { m_AssetCommands.AppendFolderCommands( out ); } );
        m_Commands.Register( "Scene", []( Out& out ) { AppendSceneCommands( out ); } );
        m_Commands.Register( "Scene (new views)",
                             [&workspace]( Out& out ) { workspace.AppendNewViewCommands( out ); } );
        m_Commands.Register( "Debug (GPU allocations)", []( Out& out ) { AppendGpuAllocationCommand( out ); } );
        m_Commands.Register( "Scene (view layout)",
                             [&workspace]( Out& out ) { workspace.AppendViewLayoutCommands( out ); } );
        m_Commands.Register( "Scene (actions)",
                             [&documents]( Out& out ) { documents.AppendFocusedDocumentCommands( out ); } );
        m_Commands.Register( "AssetCompiling",
                             [this]( Out& out ) { m_Modules.Compiling.AppendActionCommands( out ); } );
        // SAVE SCENE ANSWERS WHETHER IT SAVED. `(void)SaveOpenScene()` stood here against a
        // `[[nodiscard]] bool` — the attribute was on the declaration and the cast silenced it — so a
        // scene that could not be written came back over the channel as a success. This is the same
        // family as the toast that once said "Saved 'X'" for a file that had not been written
        // (FileSystem.hpp's note on the write primitive that is gone).
        m_Commands.Register( "SceneFiles (save)",
                             [this]( Out& out ) { m_Modules.Files.AppendSaveSceneCommand( out ); } );
        m_Commands.Register( "Play", [this]( Out& out ) { m_Modules.Play.AppendPlayCommands( out ); } );
        m_Commands.Register( "Edit (Undo, Redo)", []( Out& out ) { MainMenu::AppendUndoRedoCommands( out ); } );
        m_Commands.Register( "Documents (close all)",
                             [&documents]( Out& out ) { documents.AppendCloseAllCommand( out ); } );
        m_Commands.Register(
             "Window", [this]( Out& out )
             { DockLayout::AppendWindowCommands( out, m_Modules.Chrome ? m_Modules.App : nullptr ); } );
        m_Commands.Register( "Build", []( Out& out ) { AppendBuildCommands( out ); } );
    }

    std::vector<PaletteCommand> LevelEditorCommands::BuildPaletteCommands()
    {
        std::vector<PaletteCommand> commands;
        commands.reserve( m_Modules.Panels.Size() + m_Modules.Documents.Documents().Count() + 32 );
        m_Commands.Build( commands );
        return commands;
    }

    void LevelEditorCommands::HandleShortcuts( const ImGuiIO& io )
    {
        // ---- Global editing shortcuts ----
        // Edit mode only (Play discards its changes on Stop anyway) and never while a text field owns the
        // keyboard. Runs at frame start, before any panel iterates the scene.
        const bool editMode =
             m_Modules.Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Edit;
        if ( editMode && !io.WantTextInput && io.KeyCtrl )
        {
            if ( ::ImGui::IsKeyPressed( ImGuiKey_Z, false ) )
            {
                if ( io.KeyShift )
                    CommandHistory::Get().Redo();
                else
                    CommandHistory::Get().Undo();
            }
            if ( ::ImGui::IsKeyPressed( ImGuiKey_Y, false ) )
                CommandHistory::Get().Redo();

            if ( ::ImGui::IsKeyPressed( ImGuiKey_D, false ) )
            {
                if ( Core::SelectionManager::Count() > 0 )
                    if ( auto dups = Commands::DuplicateEntities( Core::SelectionManager::GetSelection() );
                         !dups.empty() )
                        Core::SelectionManager::SetSelection( std::move( dups ) );
            }

            if ( ::ImGui::IsKeyPressed( ImGuiKey_C, false ) && Core::SelectionManager::Count() > 0 )
                Commands::CopySelectionToClipboard( Core::SelectionManager::GetSelection() );
            if ( ::ImGui::IsKeyPressed( ImGuiKey_V, false ) )
                if ( auto pasted = Commands::PasteClipboard(); !pasted.empty() )
                    Core::SelectionManager::SetSelection( std::move( pasted ) );

            if ( ::ImGui::IsKeyPressed( ImGuiKey_N, false ) )
                m_Modules.Files.RequestNew(); // Ctrl+N -> fresh empty scene (deferred, see OnUpdate)
            // Ctrl+R -> the open scene again from its file; an untitled scene has none (the menu greys it).
            if ( ::ImGui::IsKeyPressed( ImGuiKey_R, false ) )
                (void)m_Modules.Files.RequestReload();

            if ( ::ImGui::IsKeyPressed( ImGuiKey_S, false ) )
            {
                // Deliberately discarded HERE and only here: Ctrl+S destroys nothing, so there is
                // no next step to gate. SaveOpenScene has already put the star back on and told the
                // user why if the write failed. A focused document saves its own asset instead, and
                // reports its own failure in its window.
                DocumentHost&     documents = m_Modules.Documents;
                ISubjectDocument* document  = documents.Documents().Find( documents.FocusedDocument() );
                switch ( ResolveSaveShortcut( documents.DocumentHasFocus(), document ) )
                {
                    case SaveShortcutTarget::Scene:
                        (void)m_Modules.Files.SaveOpenScene();
                        break;
                    case SaveShortcutTarget::FocusedDocument:
                        (void)document->SaveDocument();
                        break;
                    case SaveShortcutTarget::Nothing:
                        break;
                }
            }
        }

        // Command palette (Ctrl+P) — works in both edit and play modes, and even over a text field
        // so it stays reachable; the palette grabs the keyboard once open.
        if ( io.KeyCtrl && !io.KeyShift && ::ImGui::IsKeyPressed( ImGuiKey_P, false ) )
            m_Palette.Open();

        // CTRL+TAB THROUGH THE DOCUMENTS, most recently used first. This is what makes ten open
        // documents bearable: past about six the tab you want is off the end of the strip, and the
        // keyboard is the only route to it that does not involve reading a list first.
        //
        // Outside the edit-mode guard on purpose — switching document is not an edit — but not over a
        // text field, where Tab belongs to the field.
        //
        // ImGui BINDS Ctrl+Tab ITSELF (NavUpdateWindowing, enabled by NavEnableKeyboard) and it runs in
        // NewFrame, before this layer draws — so both would fire on one press: ImGui's window-ring
        // overlay AND this. The overlay is cancelled here rather than the key being fought for, and
        // ONLY when there was a document to switch to: with no documents open, Ctrl+Tab keeps ImGui's
        // ordinary window ring, which is a reasonable thing for it to do and not ours to remove.
        m_Modules.Documents.UpdateCycleShortcut( io );
    }

    void LevelEditorCommands::AppendAddShapeCommands( std::vector<PaletteCommand>& commands )
    {
        // ADD SHAPE: the outliner's Add > Shapes, one entry per authorable primitive, through the same spawn.
        for ( const Geometry::PrimitiveType type : Geometry::kAuthorablePrimitives )
        {
            commands.push_back(
                 { "Scene", std::string( "Add shape: " ) + Geometry::PrimitiveTypeName( type ), [this, type]
                   {
                       if ( !m_Modules.Workspace.ActiveScene() )
                           return PaletteCommandOutcome( false, "no scene is open" );
                       Editor::SceneHierarchyPanel::SpawnPrimitive( *m_Modules.Workspace.ActiveScene(), type );
                       return PaletteCommandDone();
                   } } );
        }
    }

    void LevelEditorCommands::AppendPaletteDoorCommand( std::vector<PaletteCommand>& commands )
    {
        // THE PALETTE'S OWN DOOR. Ctrl+P is the only other way to it and a keystroke is not available to
        // this machine, so the command palette was the single window in this editor that no unattended run
        // could put on screen — and therefore the one whose appearance no change to it could ever be
        // checked against. Г14's rule reaches its own instrument: a capability reachable only by hand does
        // not exist for the channel. Found by needing it, exactly as the snap steps and the entity delete
        // were: A6-1 changed WHEN this list is built and could not photograph the result.
        commands.push_back( { "View", "Open the command palette", [this]
                              {
                                  m_OpenPaletteRequested = true;
                                  return PaletteCommandDone();
                              } } );
    }

    void LevelEditorCommands::AppendSceneCommands( std::vector<PaletteCommand>& commands )
    {
        // THE LEVELS, which every other kind of document could already be opened by name from here and a
        // level could not — the one thing an editor exists to open was the one thing the palette had no
        // entry for, and therefore the one thing the control channel could not ask for either (the
        // channel's vocabulary IS this list). A separate group from "Open" above because these are not
        // documents: opening one REPLACES the world rather than adding a tab.
        //
        // Routed through SceneOpenRequest, not through LoadScene, on purpose: that is the path that runs
        // the unsaved-changes gate, and a palette entry is at least as easy to hit by accident as the
        // drag-and-drop it was written for.
        SceneFiles::AppendOpenSceneCommands( commands );

        // The Level Viewport commands the F / Esc keys run, on the viewport the user works in.
        for ( const Editor::ViewportCommand command : Editor::kViewportCommandOrder )
            commands.push_back( { std::string( Editor::CommandInfo( command ).Context ),
                                  std::string( Editor::CommandInfo( command ).Label ),
                                  std::bind_front( &Editor::ViewportPanel::RequestCommand, command ) } );
    }

    void LevelEditorCommands::DrawPalette()
    {
        // THE OVERLAY ITSELF, ASKED FOR BY NAME. Ctrl+P is the only other way in, and a keystroke is not
        // available to this machine — so the command palette was the one window in this editor that no
        // unattended run could photograph, which made every change to it unverifiable. Г14's rule applied
        // to the palette's own door: a capability reachable only by hand does not exist for the channel.
        //
        // A DEFERRED FLAG rather than calling Open() in the closure, and the reason is the one asymmetry
        // that would otherwise make this a knob that does nothing. CommandPalette::Draw runs the chosen
        // entry and then sets m_Open = false on the very next line, so an entry that opened the palette
        // from inside the palette would be closed again before the frame ended — working over the socket
        // and doing nothing under a person's hand. Consumed below, in this same frame, so the channel's
        // ordering guarantee still holds: the frame that answers the command is the frame that shows it.
        if ( m_OpenPaletteRequested )
        {
            m_OpenPaletteRequested = false;
            m_Palette.Open();
        }

        // BUILT ON THE FRAME IT OPENS, AND NOT ON EVERY FRAME IT IS OPEN.
        //
        // The `Open` group is enumerated from the project's FILES rather than from the asset manager's cache,
        // so a per-frame rebuild would be a recursive walk of the content tree sixty times a second while
        // somebody types a query (and the scene list beside it, CollectAvailableScenes, walks a directory tree
        // too).
        //
        // Rebuilding on OPEN is not a snapshot going stale, and that is why this is the fix rather than a
        // cache: the palette takes the keyboard while it is up, so nothing can open a document, load a
        // scene or delete an entity between the build and the choice. Running an entry closes it, and the
        // next Ctrl+P builds again.
        if ( m_Palette.TakeJustOpened() )
            m_Palette.SetCommands( BuildPaletteCommands() );

        if ( !m_Palette.IsOpen() )
            return;

        // AND THE PERSON WHO CLICKED HEARS IT TOO. The palette hands back what the chosen entry answered
        // (A6-2 point 1); before that, a command picked from Ctrl+P that failed simply closed the overlay
        // and left the editor looking as though it had obeyed. The toast is raised HERE and not inside
        // CommandPalette so that class keeps one UI dependency instead of two — the owner of the palette
        // owns how a refusal is shown.
        if ( const auto chosen = m_Palette.Draw(); !chosen )
            Editor::ToastManager::Push( chosen.GetError(), Editor::ToastLevel::Error );
    }
} // namespace Desert::Editor

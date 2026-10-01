// SCENE FILE DIALOGS — the Scenes menu, File -> Open Scene, the Open Scene modal and the unsaved-changes
// confirm. Members of SceneFiles (SceneFiles.hpp); moved out of EditorLayer.cpp unchanged (EDL-4).
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/Core/IconsMaterialDesignIcons.hpp"
#include "Editor/Core/SceneOpenRequest.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/Core/ImGuiUtilities.hpp"
#include <ImGui/imgui.h>
#include <algorithm> // std::sort / std::transform (scene list)
#include <cctype>    // std::tolower (scene filter)

namespace Desert::Editor
{
    // Case-insensitive matching for the scene filter (ASCII: scene paths on disk are ASCII).
    static std::string Lowercased( const std::string& text )
    {
        std::string out = text;
        std::transform( out.begin(), out.end(), out.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return out;
    }

    void SceneFiles::DrawDialogs()
    {
        DrawOpenScenePopup();
        DrawConfirmOpenScenePopup();
        DrawSaveScenePopup();
    }

    void SceneFiles::DrawOpenSceneMenuItem()
    {
        namespace ImGui = ::ImGui;

        if ( ImGui::MenuItem( "Open Scene" ) )
        {
            PrepareScenePopup();
            m_OpenScenePopup = true;
        }
    }

    void SceneFiles::DrawScenesMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Scenes" ) )
        {
            return;
        }

        if ( ImGui::MenuItem( "Load Scene..." ) )
        {
            PrepareScenePopup();
            m_OpenScenePopup = true;
        }

        // Opening and closing scene VIEWS moved to View -> Viewports, next to the Scene panel's own toggle.
        // This menu is about scene FILES; a viewport is not one, and two menus offering the same New Scene
        // View was two places to keep in step for one action.

        if ( !m_RecentScenes.empty() )
        {
            ImGui::Separator();
            ImGui::TextDisabled( "Recent Scenes" );

            for ( const auto& path : m_RecentScenes )
            {
                const std::string label = Label( path );
                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    // Same gated path as the palette and the Open Scene dialog (see SceneOpenRequest).
                    Editor::Core::SceneOpenRequest::Request( path.string() );
                }
                Utils::ImGuiUtilities::Tooltip( path.string().c_str() );
            }
        }

        ImGui::EndMenu();
    }

    void SceneFiles::DrawOpenScenePopup()
    {
        namespace ImGui = ::ImGui;

        if ( m_OpenScenePopup )
        {
            ImGui::OpenPopup( "Open Scene" );
            m_OpenScenePopup = false;
        }

        ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                 ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( "Open Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "Select Scene" );
            ImGui::Separator();

            ImGui::SetNextItemWidth( 450.0f );
            ImGui::InputTextWithHint( "##SceneFilter", ICON_MDI_MAGNIFY " Filter", m_SceneFilter,
                                      sizeof( m_SceneFilter ) );

            ImGui::BeginChild( "SceneList", ImVec2( 450, 300 ), true );

            const std::string filter  = Lowercased( m_SceneFilter );
            bool              loadNow = false; // double-click = pick AND load, in one gesture
            std::string       shownFolder;     // last folder header drawn
            bool              haveFolder = false;
            bool              anyShown   = false;

            for ( int i = 0; i < static_cast<int>( m_AvailableScenes.size() ); ++i )
            {
                const std::string label = Label( m_AvailableScenes[i] );
                if ( !filter.empty() && Lowercased( label ).find( filter ) == std::string::npos )
                    continue;

                // Split "Folder/Sub/Scene.desce" into its folder header and the scene's own name.
                const size_t      slash  = label.find_last_of( '/' );
                const std::string folder = slash == std::string::npos ? std::string() : label.substr( 0, slash );
                const std::string name   = slash == std::string::npos ? label : label.substr( slash + 1 );

                if ( !haveFolder || folder != shownFolder )
                {
                    if ( anyShown )
                        ImGui::Spacing();
                    if ( folder.empty() )
                        ImGui::TextDisabled( ICON_MDI_FOLDER_HOME " Scenes" );
                    else
                        ImGui::TextDisabled( ICON_MDI_FOLDER " %s", folder.c_str() );
                    shownFolder = folder;
                    haveFolder  = true;
                }

                anyShown = true;

                ImGui::PushID( i ); // two folders may hold the same filename
                ImGui::Indent( 12.0f );
                if ( ImGui::Selectable( name.c_str(), m_SelectedSceneIndex == i,
                                        ImGuiSelectableFlags_AllowDoubleClick ) )
                {
                    m_SelectedSceneIndex = i;
                    if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                        loadNow = true;
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", m_AvailableScenes[i].string().c_str() );
                ImGui::Unindent( 12.0f );
                ImGui::PopID();
            }

            if ( !anyShown )
                ImGui::TextDisabled( m_AvailableScenes.empty() ? "No scenes found" : "No match" );

            ImGui::EndChild();

            ImGui::Separator();

            const bool hasSelection =
                 m_SelectedSceneIndex >= 0 && m_SelectedSceneIndex < static_cast<int>( m_AvailableScenes.size() );

            if ( ImGui::Button( "Load", ImVec2( 120, 0 ) ) || loadNow )
            {
                if ( hasSelection )
                {
                    // The palette's path, not LoadScene: this button used to skip the unsaved-changes gate
                    // that the palette, the drop and the asset browser all run, and discarded edits silently.
                    Editor::Core::SceneOpenRequest::Request( m_AvailableScenes[m_SelectedSceneIndex].string() );
                }

                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if ( ImGui::Button( "Cancel", ImVec2( 120, 0 ) ) )
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // Guard for the scene handed over by a panel (viewport drop / asset double-click): the document is
    // about to be replaced, and unlike the menu path this can be triggered by a slip of the mouse. Only
    // shown when there is something to lose — a clean scene opens straight away.
    void SceneFiles::DrawConfirmOpenScenePopup()
    {
        namespace ImGui = ::ImGui;

        if ( m_ConfirmOpenScenePopup )
        {
            ImGui::OpenPopup( "Open Scene?" );
            m_ConfirmOpenScenePopup = false;
            // A failure belongs to the attempt that produced it. Without this a save that failed once
            // would keep warning about a scene the user has since saved by hand.
            m_SaveAndOpenError.clear();
        }

        ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                 ImVec2( 0.5f, 0.5f ) );

        if ( !ImGui::BeginPopupModal( "Open Scene?", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;

        const bool havePending = m_PendingOpenScene.has_value();

        ImGui::TextUnformatted( "The current scene has unsaved changes." );
        ImGui::TextDisabled( "Open %s", havePending ? Label( *m_PendingOpenScene ).c_str() : "" );

        // A failed "Save and Open" from a previous click of this same modal. It is shown INSIDE the
        // modal rather than only as a toast because the buttons below are still live: the user is about
        // to decide whether to discard this scene, and that decision changes completely once the save
        // they asked for turns out not to have happened.
        if ( !m_SaveAndOpenError.empty() )
        {
            ImGui::Separator();
            ImGui::TextColored( ThemeManager::GetErrorColor(), "%s", m_SaveAndOpenError.c_str() );
            ImGui::TextDisabled( "\"Discard\" below would throw these changes away for good." );
        }

        ImGui::Separator();

        if ( ImGui::Button( "Save and Open", ImVec2( 130, 0 ) ) )
        {
            // THE GATE THIS WHOLE TASK EXISTS FOR. LoadScene below clears the command history and calls
            // m_Workspace.ActiveScene()->Clear() — it destroys the only copy of the work the user just asked to
            // have saved. Before the save chain returned a result this ran unconditionally, so a scene that failed
            // to reach the disk was then deleted from memory, with a green "Saved" toast over it and nowhere to
            // recover from. The modal now stays open on a failed write and says so.
            if ( SaveOpenScene() )
            {
                m_SaveAndOpenError.clear();
                if ( havePending )
                    RequestLoad( *m_PendingOpenScene );
                m_PendingOpenScene.reset();
                ImGui::CloseCurrentPopup();
            }
            else
            {
                m_SaveAndOpenError = "The scene was NOT saved — see the log for the failing step. "
                                     "Nothing has been opened and nothing has been thrown away.";
            }
        }

        ImGui::SameLine();

        if ( ImGui::Button( "Discard", ImVec2( 110, 0 ) ) )
        {
            m_SaveAndOpenError.clear();
            if ( m_PendingOpenScene.has_value() )
            {
                RequestLoad( *m_PendingOpenScene );
            }
            m_PendingOpenScene.reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if ( ImGui::Button( "Cancel", ImVec2( 110, 0 ) ) )
        {
            m_SaveAndOpenError.clear();
            m_PendingOpenScene.reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void SceneFiles::DrawSaveScenePopup()
    {
        if ( !m_SaveSceneRequested )
        {
            return;
        }

        m_SaveSceneRequested = false;
        // Discarded for the same reason as Ctrl+S: File -> Save destroys nothing, and SaveOpenScene has
        // already reported the outcome and left the unsaved mark standing if the write failed.
        (void)SaveOpenScene();
    }
} // namespace Desert::Editor

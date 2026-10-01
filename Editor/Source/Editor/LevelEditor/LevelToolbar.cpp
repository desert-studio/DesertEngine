#include "Editor/LevelEditor/LevelToolbar.hpp"

#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/EditorResources.hpp"
#include "Editor/Core/GizmoState.hpp"
#include "Editor/Core/ImGuiUtilities.hpp"
#include "Editor/Core/PanelRequests.hpp"
#include "Editor/Core/ProjectContext.hpp"
#include "Editor/Core/Selection/ViewportMode.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/PreferencesWindow.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/ViewportCommands.hpp" // kGridSteps / kAngleSteps: the snap lists
#include "Editor/Widgets/ToolbarLayout.hpp"
#include <Common/Core/Logger.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Engine/Core/Scene.hpp>
#include <ImGui/imgui.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace Desert::Editor
{
    void LevelToolbar::DrawProjectSection()
    {
        namespace ImGui = ::ImGui;

        ImGui::PushFont( Editor::EditorResources::GetBoldFont() );

        ImGui::SameLine( ImGui::GetCursorPosX() + 40.0f );
        ImGui::Separator();
        ImGui::SameLine();

        // The PROJECT name (from the .deproj), not the working directory ("Editor" told you nothing).
        ImGui::TextUnformatted( Editor::ProjectContext::Current().Name.c_str() );
        Utils::ImGuiUtilities::Tooltip( Editor::ProjectContext::FilePath().c_str() );

        // Build configuration badge — you always want to know which binary you are looking at.
#ifdef DESERT_CONFIG_DEBUG
        constexpr const char* kConfig      = "DEBUG";
        const ImVec4          configColour = ImVec4( 0.95f, 0.65f, 0.25f, 1.0f );
#else
        constexpr const char* kConfig      = "RELEASE";
        const ImVec4          configColour = ImVec4( 0.35f, 0.85f, 0.45f, 1.0f );
#endif
        ImGui::SameLine();
        ImGui::TextColored( configColour, "[%s]", kConfig );

        ImGui::SameLine();
        ImGui::Separator();

        ImGui::PopFont();
    }

    void LevelToolbar::DrawSceneRenameSection()
    {
        namespace ImGui = ::ImGui;

        ImGui::SameLine( ImGui::GetCursorPosX() + 32.0f );

        if ( !m_RenamingScene )
        {
            // A SELECTABLE, NOT TEXT, and the difference is not cosmetic. ImGui gives a plain text item the
            // id 0, so IsAnyItemHovered() is FALSE while the cursor is over it — and the bar is now the
            // window's title bar, whose empty space is "drag the window" and whose empty space double-
            // clicked is "maximize". Left as text, this name would have been empty space: a double click
            // meant to rename the level would have renamed it AND maximized the window at the same time,
            // and a drag from it would have carried the window off. An id also buys the hover highlight,
            // which is the affordance the tooltip was standing in for.
            const std::string& sceneName = m_Workspace.ActiveScene()->GetSceneName();
            const ImVec2       nameSize  = ImGui::CalcTextSize( sceneName.c_str() );
            ImGui::Selectable( sceneName.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                               ImVec2( nameSize.x, 0.0f ) );

            if ( ImGui::IsItemHovered() )
            {
                ImGui::SetTooltip( "Double-click to rename the scene" );
                if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                {
                    m_RenamingScene   = true;
                    m_SceneNameBuffer = sceneName;
                }
            }
        }
        else
        {
            ImGui::SetNextItemWidth( 200.0f );
            Utils::ImGuiUtilities::InputText( m_SceneNameBuffer, "##SceneRename" );

            if ( ImGui::IsItemDeactivatedAfterEdit() )
            {
                if ( !m_SceneNameBuffer.empty() )
                    m_Workspace.ActiveScene()->SetSceneName( m_SceneNameBuffer );

                m_RenamingScene = false;
            }

            if ( ImGui::IsKeyPressed( ImGuiKey_Escape ) )
            {
                m_RenamingScene = false;
            }
        }
    }

    // One toolbar button: an icon, an optional label, and an "armed" state that is drawn as a tinted fill
    // plus a 2px underline. The underline matters — a tint alone is ambiguous against a hover, and the
    // question "which mode am I in" has to be answerable from across the room.
    namespace
    {
        // The one spelling of a toolbar button's text: ToolbarButton draws it and ToolbarButtonWidth
        // measures it, so the width the layout reserves is the width the button takes.
        struct ToolbarButtonText
        {
            std::string Text;

            ToolbarButtonText( const char* icon, const char* label )
                 : Text( label != nullptr && label[0] != '\0' ? std::format( "{}  {}", icon, label )
                                                              : std::string( icon ) )
            {
            }
        };

        // Must be called under the toolbar's own FramePadding (DrawToolbar pushes it).
        float ToolbarButtonWidth( const char* icon, const char* label )
        {
            const ToolbarButtonText text( icon, label );
            return ::ImGui::CalcTextSize( text.Text.c_str() ).x + ::ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        // A snap control's face: the step it reports and its icon. One spelling, read by DrawSnapControl to
        // draw the button and by DrawToolbar to measure the left groups before drawing them.
        struct SnapButtonFace
        {
            const char* Icon;
            std::string Label;
        };

        SnapButtonFace SnapFace( const bool rotation )
        {
            using Gz = ::Desert::Editor::Core::GizmoState;
            if ( rotation )
                return { ICON_MDI_ANGLE_ACUTE, std::format( "{:.0f}\xC2\xB0", Gz::RotateSnapDegrees() ) };
            const char* icon = Gz::PersistentSnap() ? ICON_MDI_MAGNET_ON : ICON_MDI_MAGNET;
            if ( Gz::TranslateSnap() >= 100.0f )
                return { icon, std::format( "{:.0f} m", Gz::TranslateSnap() / 100.0f ) };
            return { icon, std::format( "{:.0f} cm", Gz::TranslateSnap() ) };
        }
    } // namespace

    bool LevelToolbar::ToolbarButton( const char* icon, const char* label, bool active, const char* tooltip,
                                      bool enabled )
    {
        namespace ImGui = ::ImGui;

        const ToolbarButtonText button( icon, label );
        const char*             text = button.Text.c_str();

        const ImVec4 accent = ThemeManager::GetSelectedColor();
        ImGui::PushStyleColor( ImGuiCol_Button, active ? ImVec4( accent.x, accent.y, accent.z, 0.30f )
                                                       : ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 1.0f, 1.0f, 1.0f, 0.09f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 1.0f, 1.0f, 1.0f, 0.16f ) );
        ImGui::PushStyleColor( ImGuiCol_Text,
                               active ? ImGui::GetStyleColorVec4( ImGuiCol_Text ) : ThemeManager::GetIconColor() );
        if ( !enabled )
            ImGui::BeginDisabled();

        const bool clicked = ImGui::Button( text );

        if ( active )
        {
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( mn.x, mx.y - 2.0f ), mx,
                                                       ImGui::GetColorU32( accent ) );
        }
        if ( !enabled )
            ImGui::EndDisabled();
        ImGui::PopStyleColor( 4 );

        if ( tooltip != nullptr && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "%s", tooltip );
        return clicked;
    }

    void LevelToolbar::ToolbarSeparator()
    {
        namespace ImGui = ::ImGui;
        ImGui::SameLine( 0.0f, 8.0f );
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddLine( ImVec2( p.x, p.y + 3.0f ), ImVec2( p.x, p.y + h - 3.0f ),
                                             IM_COL32( 70, 70, 70, 255 ) );
        ImGui::SameLine( 0.0f, 9.0f );
    }

    void LevelToolbar::Draw()
    {
        namespace ImGui = ::ImGui;
        using Gz        = ::Desert::Editor::Core::GizmoState;
        using Mode      = ::Desert::Editor::Core::ViewportMode;
        using EMode     = ::Desert::Editor::Core::EditorMode;

        // THE STRIP HAS WORK NOW.
        //
        // It used to hold two playback buttons hard against the right edge and about 900px of nothing, and
        // the comment here argued that a second row of commands was "more chrome between the menu and the
        // picture". That was true of a DUPLICATE row. What the owner approved instead is the row UE
        // actually ships: the four things that are true of the whole editor rather than of one panel —
        // what you can undo, what mode you are in, how the gizmo behaves, and whether the world is
        // running — none of which had a home. Editor MODES in particular could only be reached from a
        // combo inside the viewport's own strip, which is the one place you cannot see while looking at
        // another panel.
        //
        // Everything here drives state that already exists and already has exactly one owner: CommandHistory,
        // ViewportMode, GizmoState, Scene::GetState. No control on this bar holds a value of its own.
        const float barHeight = ImGui::GetFrameHeight() + 12.0f;

        ImGui::PushStyleColor( ImGuiCol_ChildBg, ImVec4( 0.086f, 0.086f, 0.086f, 1.0f ) ); // #161616 strip
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8.0f, 4.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 2.0f, 0.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 5.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 4.0f );
        ImGui::BeginChild( "##Toolbar", ImVec2( 0.0f, barHeight ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );

        const bool editMode = m_Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Edit;

        // ---- Measure before drawing: the playback group sits on the bar's MIDDLE in every window ------
        // The left groups are measured both labelled and icon-only (the same text measure ToolbarButton
        // draws with); when their labels would push the playback group off the middle, the labels collapse
        // to icons (UE's toolbar entries drop labels before the bar clips) and the tooltips name each button.
        // THIS LIST IS THE LEFT GROUPS' DRAW ORDER BELOW, button for button: the drift check after the snap
        // controls compares it with the row the buttons really took.
        namespace Layout = ::Desert::Editor::ToolbarLayout;
        struct RightButton
        {
            const char* Icon;
            const char* Label;
        };
        const std::array<RightButton, 3> right   = { { { ICON_MDI_PACKAGE_VARIANT_CLOSED, "Package" },
                                                       { ICON_MDI_MONITOR_DASHBOARD, "Profiler" },
                                                       { ICON_MDI_COG, "Settings" } } };
        const float                      spacing = ImGui::GetStyle().ItemSpacing.x;
        float                            rightW  = spacing * static_cast<float>( right.size() - 1 );
        for ( const RightButton& button : right )
            rightW += ToolbarButtonWidth( button.Icon, button.Label );

        const float                 frameH      = ImGui::GetFrameHeight();
        const Layout::PlaybackGroup probe       = Layout::LayoutPlaybackGroup( frameH );
        const ImVec2                origin      = ImGui::GetWindowPos();
        const float                 contentMinX = origin.x + ImGui::GetWindowContentRegionMin().x;
        const float                 contentMaxX = origin.x + ImGui::GetWindowContentRegionMax().x;

        const bool           spaceLocal = Gz::EffectiveSpace( Gz::Get() ) == Gz::Space::Local;
        const SnapButtonFace gridFace   = SnapFace( /*rotation=*/false );
        const SnapButtonFace angleFace  = SnapFace( /*rotation=*/true );
        const auto measure = []( const char* icon, const char* text, bool afterSeparator, bool collapsible )
        {
            const float labelled = ToolbarButtonWidth( icon, text );
            return Layout::LeftButton{ labelled, collapsible ? ToolbarButtonWidth( icon, "" ) : labelled,
                                       afterSeparator };
        };
        const std::array<Layout::LeftButton, 13> leftButtons = { {
             measure( ICON_MDI_CONTENT_SAVE, "Save", false, true ),
             measure( ICON_MDI_UNDO, "", false, false ),
             measure( ICON_MDI_REDO, "", false, false ),
             measure( ICON_MDI_CURSOR_DEFAULT_OUTLINE, "Select", true, true ),
             measure( ICON_MDI_CUBE_OUTLINE, "Modeling", false, true ),
             measure( ICON_MDI_GRASS, "Foliage", false, true ),
             measure( ICON_MDI_TERRAIN, "Landscape", false, true ),
             measure( ICON_MDI_CURSOR_MOVE, "", true, false ),
             measure( ICON_MDI_ROTATE_ORBIT, "", false, false ),
             measure( ICON_MDI_ARROW_EXPAND_ALL, "", false, false ),
             measure( spaceLocal ? ICON_MDI_AXIS_ARROW : ICON_MDI_EARTH, spaceLocal ? "Local" : "World", false,
                      true ),
             measure( gridFace.Icon, gridFace.Label.c_str(), true, false ),
             measure( angleFace.Icon, angleFace.Label.c_str(), false, false ),
        } };
        const bool                               compact =
             Layout::ChooseLeftLabels( Layout::Row{
                  .ContentMinX = contentMinX,
                  .ContentMaxX = contentMaxX,
                  .LeftEnd     = contentMinX + Layout::LeftGroupsWidth( leftButtons, spacing, /*compact=*/false ),
                  .CentreWidth = probe.Width,
                  .RightWidth  = rightW,
             } ) == Layout::LeftLabels::IconsOnly;
        const auto label = [compact]( const char* text ) { return compact ? "" : text; };

        // ---- Left: the file/history group -------------------------------------------------------
        const bool dirty = m_SceneFiles.HasUnsavedChanges();
        if ( ToolbarButton( ICON_MDI_CONTENT_SAVE, label( "Save" ), false,
                            dirty ? "Save the scene (Ctrl+S) — there are unsaved changes"
                                  : "Save the scene (Ctrl+S)" ) )
        {
            // The SAME deferred flag the File menu sets, not a second call to Serialize: saving mid-frame
            // from a toolbar and saving from a menu must be one code path, or one of them will grow a
            // step (the revision marker, a toast) the other forgets.
            m_SceneFiles.RequestSave();
        }
        ImGui::SameLine();

        const auto& undoStack = CommandHistory::Get().UndoStack();
        const auto& redoStack = CommandHistory::Get().RedoStack();
        // The tooltip NAMES the edit, which is the difference between an undo button and a dare.
        const std::string undoTip = undoStack.empty()
                                         ? std::string( "Nothing to undo" )
                                         : std::format( "Undo {} (Ctrl+Z)", undoStack.back()->GetLabel() );
        const std::string redoTip = redoStack.empty()
                                         ? std::string( "Nothing to redo" )
                                         : std::format( "Redo {} (Ctrl+Shift+Z)", redoStack.back()->GetLabel() );
        if ( ToolbarButton( ICON_MDI_UNDO, "", false, undoTip.c_str(), editMode && !undoStack.empty() ) )
            CommandHistory::Get().Undo();
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_REDO, "", false, redoTip.c_str(), editMode && !redoStack.empty() ) )
            CommandHistory::Get().Redo();
        ToolbarSeparator();

        // ---- Editor modes -----------------------------------------------------------------------
        // One button per EditorMode the editor HAS. Landscape joined when the Landscape mode landed
        // (L1-L8, LS-10..15); the stale "three modes" comment hid it from the rail while the mode was
        // reachable only through the palette. Paint is not a mode (it is the Landscape Paint tab).
        const EMode mode = Mode::Get();
        if ( ToolbarButton( ICON_MDI_CURSOR_DEFAULT_OUTLINE, label( "Select" ), mode == EMode::Select,
                            "Select — selection and transform tools" ) )
            Mode::Set( EMode::Select );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_CUBE_OUTLINE, label( "Modeling" ), mode == EMode::Modeling,
                            "Modeling — geometry tools (CubeGrid blockout)" ) )
            Mode::Set( EMode::Modeling );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_GRASS, label( "Foliage" ), mode == EMode::Foliage,
                            "Foliage — paint instanced vegetation" ) )
            Mode::Set( EMode::Foliage );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_TERRAIN, label( "Landscape" ), mode == EMode::Landscape,
                            "Landscape — create, sculpt and paint landscapes" ) )
            Mode::Set( EMode::Landscape );
        ToolbarSeparator();

        // ---- Transform tools --------------------------------------------------------------------
        //
        // THE KEYS NAMED HERE ARE THE KEYS THAT WORK. These three tooltips read "(W)", "(E)" and "(R)"
        // — UE's bindings — while the only handler in the editor binds T, R and C
        // (ViewportPanel::OnKeyPressed). So the rail advertised three shortcuts that did nothing,
        // and the viewport strip's own tooltips (Move (T) / Rotate (R) / Scale (C)) said the true thing
        // eight inches away. A UI string is a promise about the tree, and this one was not kept.
        //
        // Corrected toward the CODE rather than toward UE, deliberately: adopting W/E/R is a shortcut
        // decision with a Foliage/Modeling conflict to weigh and belongs to whoever owns the keymap, not
        // to a tooltip edit. Naming the working key costs nothing and is true today either way.
        const Gz::Operation op = Gz::Get();
        if ( ToolbarButton( ICON_MDI_CURSOR_MOVE, "", op == Gz::Operation::Translate, "Translate (T)" ) )
            Gz::Set( Gz::Operation::Translate );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_ROTATE_ORBIT, "", op == Gz::Operation::Rotate, "Rotate (R)" ) )
            Gz::Set( Gz::Operation::Rotate );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_ARROW_EXPAND_ALL, "", op == Gz::Operation::Scale, "Scale (C)" ) )
            Gz::Set( Gz::Operation::Scale );
        ImGui::SameLine();

        // ---- Transform space -------------------------------------------------------------------
        // One button that both REPORTS the space and flips it, the same bargain the snap controls make
        // below. It asks EffectiveSpace(), not GetSpace(), because ImGuizmo throws the mode away while
        // scaling (ImGuizmo.cpp:2653) — so during a Scale the honest thing to show is Local, disabled,
        // rather than a "World" the handles will not honour. The button that lies is worse than the
        // button that is greyed out, and this is the only place the two could have drifted apart.
        {
            const bool      forced  = Gz::SpaceIsForced( op );
            const Gz::Space space   = Gz::EffectiveSpace( op );
            const bool      isLocal = space == Gz::Space::Local;

            const char* tip = nullptr;
            if ( forced )
                tip = "Scaling is always along the object's own axes — a world-axis scale of a rotated object "
                      "is a shear, which a transform cannot hold";
            else if ( isLocal )
                tip = "Transform space: Local — drag along the object's own axes (click for World)";
            else
                tip = "Transform space: World — drag along the world axes (click for Local)";

            if ( ToolbarButton( isLocal ? ICON_MDI_AXIS_ARROW : ICON_MDI_EARTH,
                                label( isLocal ? "Local" : "World" ), isLocal, tip, /*enabled=*/!forced ) )
                Gz::SetSpace( isLocal ? Gz::Space::World : Gz::Space::Local );
        }
        ToolbarSeparator();

        // ---- The two snap values ----------------------------------------------------------------
        // Each button both REPORTS its step and opens the list that changes it, and the shared magnet
        // toggle sits at the top of both lists rather than becoming a third button: snapping is one state,
        // and two buttons for it would be two places to read a single yes/no.
        DrawSnapControl( /*rotation=*/false );
        ImGui::SameLine();
        DrawSnapControl( /*rotation=*/true );

        // ---- Centre: playback; Right: the things you leave the editor through ---------------------
        // ONE placement for both groups (ToolbarLayout::PlaceRow), from widths that are measured rather
        // than assumed: the playback group's width depends on the frame height alone (its slots grey out,
        // they never disappear), and the right group's is the sum of its buttons' own labels. The centre
        // group sits on the bar's middle whatever the scene state and whatever the left groups read
        // (their labels collapse first, above), and slides only when even their icons would be under it.
        {
            const float rowY    = ImGui::GetItemRectMin().y;
            const float leftEnd = ImGui::GetItemRectMax().x;

            // The measured list above must be the row the buttons really took, or the middle is a guess.
            if ( const float measured = contentMinX + Layout::LeftGroupsWidth( leftButtons, spacing, compact );
                 std::abs( measured - leftEnd ) > 1.0f )
            {
                static bool reported = false;
                if ( !std::exchange( reported, true ) )
                    LOG_ERROR( "[Toolbar] the left groups' measure ({:.1f}) is not the row they drew ({:.1f}); "
                               "DrawToolbar's leftButtons list has drifted from its buttons",
                               measured, leftEnd );
            }

            const Layout::RowPlacement row = Layout::PlaceRow( Layout::Row{
                 .ContentMinX = contentMinX,
                 .ContentMaxX = contentMaxX,
                 .LeftEnd     = leftEnd,
                 .CentreWidth = probe.Width,
                 .RightWidth  = rightW,
            } );

            m_Play.DrawPlaybackGroup( Layout::LayoutPlaybackGroup( frameH, row.CentreX ), rowY );

            ImGui::SetCursorScreenPos( ImVec2( row.RightX, rowY ) );
            if ( ToolbarButton( right[0].Icon, right[0].Label, false, "Build and package the project" ) )
                Core::PanelRequests::Open( "Build Settings" );
            ImGui::SameLine();
            if ( ToolbarButton( right[1].Icon, right[1].Label, m_ShowProfiler, "Per-pass CPU and GPU timings" ) )
                m_ShowProfiler = !m_ShowProfiler;
            ImGui::SameLine();
            if ( ToolbarButton( right[2].Icon, right[2].Label, m_Preferences.IsOpen(), "Editor preferences" ) )
                m_Preferences.Toggle();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar( 4 );
        ImGui::PopStyleColor();
    }

    void LevelToolbar::DrawSnapControl( bool rotation )
    {
        namespace ImGui = ::ImGui;
        using Gz        = ::Desert::Editor::Core::GizmoState;

        // The steps are declared once at the top of this file, because the command palette offers exactly
        // these and a second copy here is how the two lists would drift apart.

        const SnapButtonFace face = SnapFace( rotation );
        const char*          tip  = rotation ? "Angle snap — click to change the step or toggle snapping"
                                             : "Grid snap — click to change the step or toggle snapping";
        if ( ToolbarButton( face.Icon, face.Label.c_str(), Gz::PersistentSnap(), tip ) )
            ImGui::OpenPopup( rotation ? "##AngleSnapPopup" : "##GridSnapPopup" );

        if ( ImGui::BeginPopup( rotation ? "##AngleSnapPopup" : "##GridSnapPopup" ) )
        {
            bool snapping = Gz::PersistentSnap();
            if ( ImGui::Checkbox( "Snapping", &snapping ) )
                Gz::SetPersistentSnap( snapping );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Holding Ctrl while dragging inverts this." );
            ImGui::Separator();

            if ( rotation )
            {
                for ( const float step : kAngleSteps )
                {
                    char item[32];
                    std::snprintf( item, sizeof( item ), "%.0f\xC2\xB0", step );
                    if ( ImGui::Selectable( item, Gz::RotateSnapDegrees() == step ) )
                        Gz::SetRotateSnapDegrees( step );
                }
            }
            else
            {
                for ( const float step : kGridSteps )
                {
                    char item[32];
                    if ( step >= 100.0f )
                        std::snprintf( item, sizeof( item ), "%.0f m", step / 100.0f );
                    else
                        std::snprintf( item, sizeof( item ), "%.0f cm", step );
                    if ( ImGui::Selectable( item, Gz::TranslateSnap() == step ) )
                        Gz::SetTranslateSnap( step );
                }
            }
            ImGui::EndPopup();
        }
    }
} // namespace Desert::Editor

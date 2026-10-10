#include "Editor/LevelEditor/PreferencesWindow.hpp"

#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <ImGui/imgui.h>
#include <glm/gtc/type_ptr.hpp>

namespace Desert::Editor
{
    // THERE IS NO "SAVE" BUTTON HERE, AND ITS ABSENCE IS THE FEATURE (К8).
    //
    // Every control below binds &prefs.X and therefore edits the LIVE store — which is what the editor
    // reads every frame, so the change is already applied while the mouse is still down. The button
    // beneath them only wrote the file, and that split one decision between two deciders: closing this
    // window with the x left the edits live but unwritten (the user "cancelled" and the change kept
    // working), while ANY unrelated save — the Perf HUD toggle, an MSAA pick in Scene Settings, a star in
    // Details — silently committed those abandoned edits to disk. The button promised a decision it did
    // not take, and somebody in another panel took it.
    //
    // A gate was refused rather than overlooked. Gating would mean rewiring every CONSUMER onto a
    // committed copy, for values whose whole point is being visible while you drag the slider; a working
    // copy (as У8 built for materials) does not pay here, because a personal setting is neither authored
    // nor shared and there is nothing to revert to but what the user just saw; and three other panels
    // already persist into this same file on a click, so a gate in this one window would leave one file
    // half-gated — the same two-writers-disagreeing shape one floor up.
    //
    // So each control commits itself on ImGui::IsItemDeactivatedAfterEdit(), the pattern the viewport's
    // snap popup already uses: ONE file write when the mouse is released, not sixty a second while a drag
    // reports a change every frame. Nothing else in this window may call Save(), and
    // Desert/Tests/Editor/PreferenceOwnership reads this function to hold both halves — that every control
    // has its commit, and that no call site here saves outside one.
    void PreferencesWindow::Draw()
    {
        namespace ImGui = ::ImGui;
        if ( !m_Open )
            return;

        ImGui::SetNextWindowSize( ImVec2( 380.0f, 0.0f ), ImGuiCond_Appearing );
        if ( ImGui::Begin( "Preferences", &m_Open, ImGuiWindowFlags_NoDocking ) )
        {
            auto& prefs = EditorPreferences::Get();

            ImGui::Spacing();
            ImGui::TextDisabled( "Editor Camera" );
            ImGui::Separator();
            if ( ImGui::SliderFloat( "Speed", &prefs.CameraSpeed, 0.1f, 10.0f, "%.2fx" ) )
                if ( auto cam = m_Workspace.ActiveScene()->GetMainCamera().lock() )
                    if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( cam.get() ) )
                        editorCam->SetMovementSpeed( prefs.CameraSpeed );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();

            ImGui::Spacing();
            ImGui::TextDisabled( "Gizmo Snap" );
            ImGui::Separator();
            ImGui::Checkbox( "Snap always on (Ctrl inverts)", &prefs.PersistentSnap );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            // CENTIMETRES, which is what the value has always been fed into: this control said "(m)" and
            // clamped to 0.01..100 while writing a field GizmoState reads as world units, and a world
            // unit is 1 cm. A slider whose unit disagrees with its consumer is how the shipped grid snap
            // ended up at half a centimetre (see EditorPreferences::TranslateSnap).
            //
            // Nothing is pushed anywhere afterwards: these four ARE the snap's storage and GizmoState
            // reads them, so the gizmo follows on the same frame. The block that used to copy them into
            // GizmoState is gone with the copy it fed (К6).
            ImGui::DragFloat( "Move (cm)", &prefs.TranslateSnap, 1.0f, 1.0f, 10000.0f, "%.0f" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::DragFloat( "Rotate (deg)", &prefs.RotateSnapDeg, 0.5f, 0.1f, 180.0f, "%.1f" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::DragFloat( "Scale", &prefs.ScaleSnap, 0.01f, 0.01f, 10.0f, "%.2f" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();

            ImGui::Spacing();
            ImGui::TextDisabled( "Autosave" );
            ImGui::Separator();
            ImGui::SliderInt( "Interval (min)", &prefs.AutosaveMinutes, 0, 30,
                              prefs.AutosaveMinutes == 0 ? "Off" : "%d min" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            // Wrapped for the same reason as the footer below: at 380 px this line was clipped to
            // "...the main file is nev" and the reassurance it exists to give was the part cut off.
            ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextWrapped( "Autosaves land in <Project>/Saved/Autosaves/, the main file is never touched." );
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::TextDisabled( "Content Browser" );
            ImGui::Separator();
            // Read by ThumbnailCache on its next upload; lowering it releases the least recently drawn ones then.
            ImGui::DragInt( "Thumbnail Pool", &prefs.ThumbnailPoolSize, 8.0f, 64, 16384, "%d pictures" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();

            ImGui::Spacing();
            ImGui::TextDisabled( "Selection Outline" );
            ImGui::Separator();
            ImGui::Checkbox( "Enable Outline", &prefs.EnableOutline );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            // NoInputs: a swatch that opens the picker, not three cramped R/G/B fields. Two reasons, and
            // the first is a real collision rather than taste — with ThemeManager's
            // style.ColorButtonPosition = ImGuiDir_Left, ImGui places a ColorEdit's LABEL after the last
            // item it drew, which is the swatch and not the inputs, so the word "Color" is rendered on top
            // of the R field. (That is ImGui's own arithmetic and it affects every labelled ColorEdit in
            // this editor drawn under this theme; reported rather than fixed here, because the fix lives
            // in ThemeManager and the left-hand swatch is a deliberate UE-parity choice.) The second is
            // that three numeric fields do not fit a 380 px window, which is why this row was the one that
            // showed it.
            ImGui::ColorEdit3( "Color", glm::value_ptr( prefs.OutlineColor ), ImGuiColorEditFlags_NoInputs );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::SliderFloat( "Width (px)", &prefs.OutlineWidth, 0.0f, 20.0f );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::SliderFloat( "Smoothness", &prefs.OutlineSmoothness, 0.0f, 10.0f );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();

            ImGui::Spacing();
            // WRAPPED, not two TextDisabled lines: this window opens 380 px wide and the user may make it
            // narrower, and a fixed line is silently CLIPPED by the window edge rather than shortened —
            // the first capture of this footer read "...and is written to" with the rest gone. A sentence
            // explaining that there is no Save button is a poor sentence to lose the end of.
            ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextWrapped( "There is no Save button: every setting here applies as you change it and "
                                "is written to ~/.desertengine/editor.json when you let go of the control." );
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }
} // namespace Desert::Editor

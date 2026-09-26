#pragma once

// THE EDITOR SIDE of the Preview Scene Settings (PreviewEnvironment.hpp holds the pure rules): the one
// place that reads EditorPreferences::PreviewScene into a preview, draws its rows and offers its commands.
// Every preview window calls the same three functions, which is what makes the setting ONE setting shared
// by the Material Editor, the Static Mesh viewer and the Details mesh preview rather than three copies.

#include <Editor/Panels/IPanel.hpp>

#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class PreviewViewport;
}

namespace Desert::Editor::PreviewEnvironment
{
    // Writes the editor-wide settings onto one preview's SceneSetup. Called every frame before the
    // preview records, so an edit in one window reaches every open one on the next frame. Leaves the sky
    // dome and the skybox-material ball alone: those fills author their own sky and floor.
    void ApplyTo( PreviewViewport& preview, const Assets::AssetManager* assets );

    // The Environment rows (HDR picker, rotation, EV, Show Environment). Each finished edit is saved.
    void DrawEnvironmentRows( const Assets::AssetManager* assets );

    // The Show Floor checkbox, over the shared setting rather than the preview's own copy — a checkbox on
    // SceneSetup::ShowFloor would be overwritten by ApplyTo on the next frame.
    bool DrawShowFloor( const char* label );

    // The same edits as commands, so DesertCtl and the palette can drive them without a mouse.
    void AppendActions( std::vector<ISubjectDocument::DocumentAction>& actions,
                        const Assets::AssetManager*                    assets );
} // namespace Desert::Editor::PreviewEnvironment

#pragma once
// THE IMPORT OPTIONS WINDOW AND THE IMPORT SETTINGS SECTION (THM1l; UE: SFbxOptionWindow opened by
// UFbxFactory for a new file, and the Import Settings category of the Static Mesh Editor's Details with its
// Reimport). Both edit the ONE option set, Assets::SourceImportSettings, whose one home is the source's import
// record (Engine/Assets/Serialization/ImportRecord.hpp); both draw it with the same DrawImportSettingsFields.
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>

#include <filesystem>
#include <functional>
#include <optional>

namespace Desert::Editor
{
    class ImportManager;

    namespace ImportOptions
    {
        // The editor's one importer for user-started imports (drag-and-drop, the Import Options window,
        // Reimport). The boot cook has its own per-thread importers.
        ImportManager& SharedImporter();

        // Queues @p source for the Import Options window: the window opens on the next frame and imports it
        // with the options the user confirms. Queued twice = shown once. @p onImported (optional) runs after
        // the source imported with the confirmed options - how a viewport drop of a new file places it once the
        // window is confirmed (UE: the drop's factory import, then the actor); a cancelled or failed import
        // runs none. Every request's continuation runs, a repeated source's included.
        void Request( const std::filesystem::path& source, std::function<void()> onImported = {} );
        // True while a source waits for, or is shown in, the window.
        bool Pending();

        // THE WINDOW'S BUTTONS, one body each: the buttons call them, and so does the command palette (which is
        // how the control channel confirms the modal without a mouse). Import = the shown source with the shown
        // options; Import All = every queued source with them; Cancel = the shown source is not imported. Each
        // refuses when no source waits.
        Common::BoolResultStr ConfirmImport( bool all );
        Common::BoolResultStr CancelImport();

        // Draws the window (one modal, one source at a time: Import, Import All = the rest of the queue with the
        // same options, Cancel = this source is not imported). Call once per frame from the editor's UI pass.
        void DrawWindow();

        // The options fields both surfaces draw: Combine Meshes, Uniform Scale, Up Axis, LOD. True when the user
        // changed one this frame. Uniform Scale stays finite and > 0 (MeshSourceAsset::ValidateImport's rule).
        bool DrawImportSettingsFields( Assets::SourceImportSettings& settings );

        // The options the last Import Options window confirmed in this project (UE: the FBX import UI's
        // per-project config), read from `<project>/Saved/ImportOptions.json`. nullopt when the project has
        // never confirmed the window; an error naming the file when it exists and does not parse.
        Common::ResultStr<std::optional<Assets::SourceImportSettings>> LoadLastUsed();
        Common::BoolResultStr SaveLastUsed( const Assets::SourceImportSettings& settings );
        std::filesystem::path LastUsedPath();

        // The raw source a static mesh asset was imported from: the source beside a combined mesh
        // (`base.stmesh` -> `base.fbx`), or the file a node mesh's IMPT names (`base_TuftA.stmesh`). nullopt for a
        // mesh with no source (hand-authored, recovered).
        std::optional<std::filesystem::path> ImportSourceOfMeshAsset( const std::filesystem::path& assetPath );

        // The Details panel's "Import Settings" section for the mesh asset at @p assetPath: the source's recorded
        // options, editable, and Reimport - which imports the source again with the edited options (a changed
        // Combine Meshes changes how many static meshes the source makes). Draws nothing for a mesh with no
        // source.
        void DrawImportSettingsSection( const std::filesystem::path& assetPath );
    } // namespace ImportOptions
} // namespace Desert::Editor

#pragma once
// THE IMPORT OPTIONS WINDOW AND THE IMPORT SETTINGS SECTION (THM1l; UE: SFbxOptionWindow opened by
// UFbxFactory for a new file, and the Import Settings category of the Static Mesh Editor's Details with its
// Reimport). Both edit the ONE option set, Assets::SourceImportSettings, whose one home is the source's import
// record (Engine/Assets/Serialization/ImportRecord.hpp); both draw it with the same DrawImportSettingsFields.
#include <Common/Core/ResultStr.hpp>
#include <Editor/Import/IAssetImporter.hpp>
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
        // THE OPTIONS' EDITS, one body each: the fields of both surfaces call them, and the palette's
        // "Import Options: Uniform Scale / Up Axis / Combine Meshes" call the Shown ones on the window's options
        // (refused when no source waits). Uniform Scale refuses a value that is not finite and above zero
        // (MeshSourceAsset::ValidateImport's rule).
        Common::BoolResultStr SetUniformScale( Assets::SourceImportSettings& settings, float scale );
        void                  SetUpAxis( Assets::SourceImportSettings& settings, Assets::MeshSourceUpAxis axis );
        void                  SetCombineMeshes( Assets::SourceImportSettings& settings, bool on );
        Common::BoolResultStr SetShownUniformScale( float scale );
        Common::BoolResultStr SetShownUpAxis( Assets::MeshSourceUpAxis axis );
        Common::BoolResultStr SetShownCombineMeshes( bool on );

        // Draws the window (one modal, one source at a time: Import, Import All = the rest of the queue with the
        // same options, Cancel = this source is not imported). Call once per frame from the editor's UI pass.
        void DrawWindow();

        // The options fields both surfaces draw: Combine Meshes, Uniform Scale, Up Axis, LOD. True when the user
        // changed one this frame. Uniform Scale stays finite and > 0 (MeshSourceAsset::ValidateImport's rule).
        // Combine Meshes only for @p kind StaticMesh: a skinned file is one skeletal mesh.
        bool DrawImportSettingsFields( Assets::SourceImportSettings& settings, ImportContentKind kind );

        // The options the last Import Options window confirmed in this project (UE: the FBX import UI's
        // per-project config), read from `<project>/Saved/ImportOptions.json`. nullopt when the project has
        // never confirmed the window; an error naming the file when it exists and does not parse.
        Common::ResultStr<std::optional<Assets::SourceImportSettings>> LoadLastUsed();
        Common::BoolResultStr SaveLastUsed( const Assets::SourceImportSettings& settings );
        std::filesystem::path LastUsedPath();

        // The raw source a mesh asset was imported from: the source beside a combined static mesh or a skinned
        // mesh (`base.stmesh` / `base.skmesh` -> `base.fbx`), or the file a static node mesh's IMPT names
        // (`base_TuftA.stmesh`). nullopt for a mesh with no source (hand-authored, recovered).
        std::optional<std::filesystem::path> ImportSourceOfMeshAsset( const std::filesystem::path& assetPath );

        // What @p source's import record says it imports as (the header's Kind: StaticMesh, SkinnedMesh ->
        // SkeletalMesh, Skeleton / Animation -> Animation) - which fields its Import Settings show. An error
        // naming the record when it is missing, unreadable or states another kind.
        Common::ResultStr<ImportContentKind> RecordedImportKind( const std::filesystem::path& source );

        // The Details panel's "Import Settings" section for the mesh asset at @p assetPath (static or skinned):
        // the source's recorded options in the fields of what the record says it imports as, editable, and
        // Reimport - which imports the source again with the edited options (a changed Combine Meshes changes how
        // many static meshes the source makes; a skinned source rewrites its mesh, skeleton and clips, re-read in
        // place). Draws nothing for a mesh with no source.
        void DrawImportSettingsSection( const std::filesystem::path& assetPath );
        // The section's Reimport button, one body (the palette's "Assets / Reimport selected" runs it too): the
        // source of the mesh asset at @p assetPath imported again with the section's edited options, the
        // record's when nothing is edited. An error for a mesh with no source or a failed import.
        Common::BoolResultStr Reimport( const std::filesystem::path& assetPath );
        // UE's "Reimport with New File" (Content Browser > Asset Actions): @p newFile's bytes replace the recorded
        // source of the mesh asset at @p assetPath, then Reimport runs as above - the asset keeps its name, its
        // record and its references. The new file must be of the source's format (same extension): the asset
        // finds its source by name beside it, so a changed format would orphan the record. An error names the
        // refusal: no source, a missing new file, a format change, a failed copy or import.
        Common::BoolResultStr ReimportWithNewFile( const std::filesystem::path& assetPath,
                                                   const std::filesystem::path& newFile );
        // The section's Uniform Scale and Up Axis fields without a mouse (the palette's "Details / Import
        // Settings: ..."): the fields' own SetUniformScale / SetUpAxis on the section's working copy of the mesh
        // asset at @p assetPath's source - the copy Reimport imports with. An error for a mesh with no source or
        // record.
        Common::BoolResultStr SetSectionUniformScale( const std::filesystem::path& assetPath, float scale );
        Common::BoolResultStr SetSectionUpAxis( const std::filesystem::path& assetPath,
                                                Assets::MeshSourceUpAxis     axis );
    } // namespace ImportOptions
} // namespace Desert::Editor

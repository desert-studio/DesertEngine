// WHICH CONFIGURATION FILE OWNS WHICH SETTING — the rule, and the census that keeps it true.
//
// This engine persists settings into three authored files, and until К1 nothing anywhere said which one
// owns what. The cost of that silence is not theoretical; it has been paid four times, in four places, in
// one week:
//
//   * К2 found ten viewport debug flags living in the LEVEL file, and `ShowColliders: true` had shipped
//     through git in 55 of 73 scenes that stated it;
//   * К3 found five image-quality fields living in the level file, so a weak machine could not turn the
//     picture down without editing a file that goes to everybody. CLOSED: they are in machine.json, and
//     the sixth of the same kind (MSAASamples, in editor.json, where a shipped game could never read it)
//     went with them;
//   * У6, У8 and Д31 are the same shape one layer out — one container holding things with different
//     owners and different lifetimes.
//
// Catching instances one at a time is what this suite exists to stop. The rule is below, it decides where
// a NEW field goes without asking anyone, and the census under it is what goes red when a field lands in
// the wrong file.
//
// ===================================================================================================
// THE RULE
// ===================================================================================================
//
//   ~/.desertengine/editor.json  — ONE PERSON'S COPY OF THE EDITOR.
//       Holds what a user's own installation must remember between sessions and across every project,
//       and whose value two people on the same project may legitimately hold differently at the same
//       moment.
//       Does NOT hold anything a second person opening the project must see, and nothing the shipped
//       Runtime needs — the packaged game never opens this file.
//
//   machine.json                 — ONE HOST'S COPY OF WHAT ITS MACHINE CAN AFFORD.
//       Holds the quality knobs: MSAA, post AA, mesh LOD, the sampler's filter and anisotropy, the cloud
//       tier. Same OWNER kind as editor.json — two people may differ at the same moment — but a different
//       FILE, because the packaged game reads these and never opens editor.json. ONE SCHEMA
//       (Common::Settings::MachineSettings) served from two places: `~/.desertengine/machine.json` for
//       the editor, `<user data>/<product>/machine.json` for a shipped game. The place is a PARAMETER of
//       Load(), not a second type — two schemas obliged to agree is the defect class this project has
//       paid for repeatedly, and K3 refused to create one on purpose.
//       Does NOT hold anything about the EDITOR (that is editor.json) and nothing that describes a level.
//
//   <Name>.deproj                — WHAT THE PRODUCT IS, FOR EVERYBODY.
//       Holds the few facts every process that opens this project must agree on before any level
//       exists: its identity, where its content lives, which level boots, and the format's own version.
//       Does NOT hold anything that varies from level to level, nothing that varies from machine to
//       machine, and it grows a field only when the value genuinely cannot be derived — Constants.hpp
//       already refused fourteen folder-name fields on exactly that ground.
//
//   <Name>.desce                 — WHAT THE WORLD IS.
//       Holds the level's entities and the level-wide policy a designer authors and expects to travel
//       with the level: the render path, shadows, the grade, the lens, wind, gravity, the splash.
//       Does NOT hold what a VIEWER is doing on top of the world (К2 took ten such fields out) and does
//       NOT hold what a MACHINE can afford (К3 took five out, to machine.json).
//
// THE DECISION PROCEDURE — three questions, IN THIS ORDER. The order IS the rule.
//
//   0. Is the field the file describing ITSELF — a format version, a unit generation, the name the file
//      is filed under? Then it is FILE METADATA, it belongs to whichever file it describes, and
//      questions 1-3 do not apply. Exactly four fields may claim this kind and they are named in the
//      census below; a fifth is a conversation, not a row.
//   1. Would two people working on this project AT THE SAME TIME legitimately want different values?
//      Yes -> editor.json.
//   2. Does the value differ from one level to the next?  Yes -> .desce.
//   3. Otherwise -> .deproj.
//
// WHY QUESTION 1 COMES FIRST, which is the only part of this that is load-bearing. Questions 2 and 3 are
// both TRUE of a quality knob — anti-aliasing does differ between levels if somebody authors it that way,
// and it is a project-wide default if somebody sets it that way — so any order that asks them earlier
// finds a home for it and stops. Question 1 is the only one whose "yes" is about a CONFLICT rather than
// about a scope, and a conflict beats a scope: a value two people must be able to disagree about cannot
// live in a file they share, whatever else is true of it. Asked in the wrong order, quality settles in the
// level file — which is exactly where it is, and how it got there.
//
// HOW TO TELL A QUALITY KNOB FROM AN AUTHORED LOOK, because that is the boundary this went wrong on. Ask:
// set to its cheapest value, has the level been MIS-AUTHORED, or merely RENDERED WORSE?
//   * rendered worse -> machine quality. `MeshLOD` off is byte-identical geometry near the camera;
//     `CloudQuality` High reproduces the calibrated constants to the digit; Anisotropy 1, Nearest
//     filtering and AA None are the same picture, blurrier or harsher.
//   * mis-authored -> level data that happens to cost something. Forward and Deferred are two shading
//     models that disagree about cloud shadow on the ground; ACES and Reinhard are two grades; GI Off is
//     a darker room, not a coarser one.
// This is what settles the disagreement inside SceneSettings' own comments, where CloudQuality's note
// counts NINE cost-versus-quality siblings and the struct header names only FIVE as misplaced. The other
// four (RenderingPath, GlobalIllumination, EnableSSAO, EnableSSR) each change the authored look. The
// closest call by far is EnableSSAO, and it is called Level here deliberately rather than quietly: it is
// an on/off, not a fidelity ladder, and there is no cheaper SSAO to fall back to. If К3's machine-level
// store ever grows a scalability LEVEL, SSAO is the first field to re-examine.
//
// HOW К3 WAS ANSWERED, recorded here because this is where the question will be asked again. "editor.json"
// is not a valid home for a field the SHIPPED GAME also needs: it is an Editor-target concept
// (Desert::Editor::EditorPreferences) and the Runtime never opens it, while all five misplaced quality
// fields are read by SceneRenderer, which the packaged game runs. Moving them there would have fixed this
// repository's git history by taking the quality dial away from the player.
//
// The owner's decision (2026-09-07) was a per-machine store BOTH hosts read: ONE SCHEMA in Common, and the
// FILE as a parameter — `~/.desertengine/machine.json` for the editor, the player's own directory for a
// packaged game. The alternative considered and refused was a separate per-host file with its own struct,
// i.e. two schemas obliged to agree; that is the shape this project caught five times in one day, and a
// place is a parameter rather than a second type. This is not a fourth KIND of setting: the kind (one
// person's own copy) already existed, and what changed is that it stopped being a privilege of the editor.
//
// ===================================================================================================
// HOW THIS SUITE IS BUILT
// ===================================================================================================
//
// Each file's field list is enumerated BY THE SAME MECHANISM THAT WRITES THAT FILE, never by a hand-typed
// list — so a field added tomorrow fails here before anyone has to remember this document exists:
//
//   machine.json <- rfl::fields<MachineSettings>(), the same call MachineSettings::Save() writes it with,
//                   less its own ExtraFields carrier for the reason editor.json's is skipped.
//   editor.json  <- rfl::fields<EditorPreferences>() and rfl::fields<DebugViewState>(), which is what
//                   rfl::json::write emits in EditorPreferences::Save() — less the one member that is not
//                   a key at all, EditorPreferences::UnknownKeys, whose contents ARE keys of the file but
//                   belong to whichever build wrote them. SerializedKeysOf() below says how that is
//                   decided and which test pins it against the bytes.
//   .deproj      <- rfl::fields<ProjectFile>(), the same call Common::Project::WriteProjectFile makes.
//   .desce       <- rfl::fields<SceneSerialized>() for the top level, and the reflection registry for the
//                   Settings block, which is what SceneSerializer hands SerializeReflected.
//
// The three existing censuses in this repository ask three other questions and none of them can ask this
// one; this suite is the fourth and it deliberately does not repeat them:
//
//   SettingConsumers   "does anything READ this?"       — owns that question for every REFLECTED type,
//                                                          so the SceneSettings rows here carry no
//                                                          consumer column. It covers nothing else, which
//                                                          is why the rows for editor.json and .deproj DO
//                                                          carry one: those two files had no readership
//                                                          census at all, and К1 found two dead settings
//                                                          in editor.json the first time anyone looked.
//   SceneDebugFields   "should this be in a LEVEL?"     — the special case of this rule for one kind of
//                                                          field (debug visualization), kept because it
//                                                          also checks the migrator's key list.
//   SceneVersionGate   "will this file LOAD?"
//
// The consumer half reuses SettingConsumers' reader (setting_consumers_reader.hpp) rather than growing a
// second matcher. What is added locally is two receiver shapes that census did not need: a static
// accessor chain (`ProjectContext::Current().Name`) and a template-argument declaration
// (`std::optional<ProjectFile> s_Current;`).

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Editor/Core/EditorPreferences.hpp>

#include <Common/Project/ProjectFormat.hpp>
#include <Common/Settings/MachineSettings.hpp>

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Graphic/DebugViewState.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <rflcpp/rfl/Generic.hpp>
#include <rflcpp/rfl/Tuple.hpp>
#include <rflcpp/rfl/fields.hpp>
#include <rflcpp/rfl/internal/is_extra_fields.hpp>
#include <rflcpp/rfl/json.hpp>
#include <rflcpp/rfl/named_tuple_t.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::Reflection::FieldInfo;
using Desert::Reflection::ReflectionRegistry;

namespace
{
    // The four kinds a persisted value can have. There is no fifth, and adding one is the conversation
    // the rule above says to have rather than a row somebody slips in.
    enum class Owner
    {
        Machine,  // one person's installation; two people may differ at the same moment
        Project,  // the product, for everybody who opens it
        Level,    // the world this file describes
        FileMeta, // the file describing itself: format version, unit generation, its own name
    };

    const char* OwnerName( Owner owner )
    {
        switch ( owner )
        {
            case Owner::Machine:
                return "Machine (~/.desertengine/editor.json)";
            case Owner::Project:
                return "Project (.deproj)";
            case Owner::Level:
                return "Level (.desce)";
            case Owner::FileMeta:
                return "FileMeta (belongs to whichever file it describes)";
        }
        return "?";
    }

    struct Row
    {
        const char* Field;
        Owner       Kind;

        // A repo-relative source file that must contain an ANCHORED READ of this field (see
        // setting_consumers_reader.hpp). Left null for the SceneSettings rows on purpose: SettingConsumers
        // owns readership for every reflected type and a second table of the same 51 answers would be the
        // duplication §2.1 forbids.
        const char* Where = nullptr;
    };

    struct FileCensus
    {
        const char* File;  // the file as a person names it
        Owner       Holds; // the ONE kind this file may hold; FileMeta is additionally always allowed
        // How a consumer file is allowed to get hold of one of these values. `Type` is the struct's own
        // name; `Holder`/`Accessor` spell the static getter that hands it out (`ProjectContext::Current()`).
        const char* Type;
        const char* Holder;
        const char* Accessor;
        const Row*  Rows;
        std::size_t Count;
    };

#define CENSUS_ROWS( rows ) rows, sizeof( rows ) / sizeof( ( rows )[0] )

    // ------------------------------------------------------------------------------------------------
    // ~/.desertengine/editor.json — Desert::Editor::EditorPreferences
    //
    // Every field is Machine, and that is not an accident of this table: the file is per-USER by
    // construction (EditorPreferences::ConfigDirectory() is $HOME/.desertengine, not the project), so a
    // field of any other kind here would be a value one person's home directory decides for everybody —
    // which is unreachable for a teammate and invisible in review. If a row here ever needs a kind other
    // than Machine, the field is in the wrong file, not the row.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kEditorLayer    = "Editor/Source/EditorLayer.cpp";
    constexpr const char* kPrefsImpl      = "Editor/Source/Editor/Core/EditorPreferences.cpp";
    constexpr const char* kGizmoState     = "Editor/Source/Editor/Core/GizmoState.cpp";
    constexpr const char* kViewportPanel  = "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp";
    constexpr const char* kPhotogrammetry = "Editor/Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.cpp";
    constexpr const char* kBuildPanel     = "Editor/Source/Editor/Panels/Build/BuildSettingsPanel.cpp";

    constexpr Row kEditorPrefsRows[] = {
         // Applied to the editor camera once a camera exists.
         { "CameraSpeed", Owner::Machine, kEditorLayer },

         // The four gizmo snap values and their modifier policy. These rows named EditorPreferences.cpp
         // until К6, because Save() pushed them into Core::GizmoState — which kept its OWN copy and was
         // written directly by two toolbars, so the push reverted a live user choice on every unrelated
         // save and no toolbar write ever reached the file. The second copy is gone; GizmoState.cpp now
         // reads these four out of here and is the file this census follows them to.
         { "TranslateSnap", Owner::Machine, kGizmoState },
         { "RotateSnapDeg", Owner::Machine, kGizmoState },
         { "ScaleSnap", Owner::Machine, kGizmoState },
         { "PersistentSnap", Owner::Machine, kGizmoState },

         { "AutosaveMinutes", Owner::Machine, kEditorLayer },
         { "ShowPerfHud", Owner::Machine, kViewportPanel },

         // Which generation of the default dock layout this user has been reset to. It looks like a
         // version and is not FileMeta: it does not describe editor.json's format, it records a one-time
         // action taken on THIS installation's imgui.ini.
         { "DockLayoutVersion", Owner::Machine, kEditorLayer },

         // `MSAASamples` USED TO BE A ROW HERE, and К6 left a note on it that К3 acted on: the KIND was
         // right (a fidelity ladder, per machine) and the FILE was right only for the editor. The value
         // reached the renderer as Graphic::RenderConfig::MSAASamples, whose single writer was
         // EditorPreferences, and the packaged Runtime never opens editor.json — so a shipped game ran
         // MSAA nailed to 1 with no reader and no dial. It is censused under machine.json below, with the
         // five that came the other way, out of the level file. The row was green the whole time it was
         // wrong, which is why the note mattered more than the assertion.

         // Selection outline: an editor-only viewport visualization (a runtime build has no selection).
         { "OutlineColor", Owner::Machine, kEditorLayer },
         { "OutlineWidth", Owner::Machine, kEditorLayer },
         { "OutlineSmoothness", Owner::Machine, kEditorLayer },
         { "EnableOutline", Owner::Machine, kEditorLayer },

         // The whole view state, pushed into every scene's renderer each frame. Its ten leaves are
         // censused separately below.
         { "DebugView", Owner::Machine, kEditorLayer },

         // External photogrammetry tooling: the command lines and paths of THIS machine's installation of
         // somebody else's software. Nothing is more per-machine than a path to a binary.
         { "PhotogrammetryCommand", Owner::Machine, kPhotogrammetry },
         { "PhotogrammetryPhotosDir", Owner::Machine, kPhotogrammetry },
         { "PhotogrammetryOutputMesh", Owner::Machine, kPhotogrammetry },
         { "PhotogrammetryFaceModel", Owner::Machine, kPhotogrammetry },

         // Details-panel personalisation, saved on the click.
         { "FavouriteFields", Owner::Machine, kPrefsImpl },
         { "CollapsedComponents", Owner::Machine, kPrefsImpl },

         // The content browser's pinned folders, per project. Machine by question 1 and not a close call:
         // which folders one person keeps at hand is the definition of a value two people on the same
         // project hold differently at the same moment, and a pin is invisible to everybody else.
         //
         // IT IS THE FIFTH PER-USER STORE, AND IT WAS A FILE (К5). `~/.desertengine/asset_favorites.txt`
         // held it: flat lines, no schema, absolute paths, an `ofstream` truncation whose result nobody
         // read, and one list shared by every project this user had ever opened. Every one of those is a
         // rule this suite states — so the file was outside all of them, for the single reason that no
         // census had ever been asked whether it existed.
         //
         // The consumer is EditorPreferences.cpp rather than the browser, like its two neighbours above
         // and for the same reason: the panel is handed absolute paths by the three helpers there, and
         // which project a pin belongs to and what it is relative to are decided in this field's own file.
         { "FavouriteFolders", Owner::Machine, kPrefsImpl },

         // The three packaging answers, moved out of the Build Settings panel's own memory by П6 — they
         // were held nowhere, so every session started over. All three are Machine by question 1: an
         // output path is a place on ONE person's disk, and whether the Runtime bundled is Debug or
         // Release depends on whether that person is debugging or cutting a playtest build, which two
         // people on this project want to differ on at the same moment. Question 3's "otherwise ->
         // .deproj" never gets asked, and that is the order doing its job: an output folder does look
         // like a project-wide fact until the conflict question is asked first.
         //
         // The shipped-Runtime test the header names is also satisfied: the packaged game never reads
         // these — they are consumed by the editor BEFORE the game exists.
         //
         // The consumer named is the panel rather than GamePackager.cpp because the panel is where the
         // preference is READ (the packager is handed a PackageOptions copy). The seam beyond it —
         // preference -> PackageOptions -> packager — is Desert/Tests/Editor/BuildSettingsConsumers,
         // which asserts the relation in both directions.
         { "PackageOutputDir", Owner::Machine, kBuildPanel },
         { "PackageConfig", Owner::Machine, kBuildPanel },
         { "PackageAppBundle", Owner::Machine, kBuildPanel },
    };

    // ------------------------------------------------------------------------------------------------
    // The nested DebugView block of editor.json — Desert::Graphic::DebugViewState.
    //
    // These are the ten fields К2 took out of the level file. They are censused for KIND only: their
    // placement is additionally guarded by Desert/Tests/Engine/SceneDebugFields, which derives the set of
    // names a .desce may never state from this same declaration, and duplicating its consumer answers here
    // would be a second table of one set.
    // ------------------------------------------------------------------------------------------------

    constexpr Row kDebugViewRows[] = {
         { "ShowGrid", Owner::Machine },
         { "ShowColliders", Owner::Machine },
         { "ShowBoundingBoxes", Owner::Machine },
         { "BoundingBoxColor", Owner::Machine },
         { "BoundingBoxLineWidth", Owner::Machine },
         { "WireframeMode", Owner::Machine },
         { "ShowNormals", Owner::Machine },
         { "LightingDebug", Owner::Machine },
         { "ShadowDebug", Owner::Machine },
         { "DeferredDebug", Owner::Machine },
    };

    // ------------------------------------------------------------------------------------------------
    // machine.json — Common::Settings::MachineSettings
    //
    // Every field is Machine, and — as with editor.json — that is not an accident of this table but of the
    // file: it is per-HOST by construction, written into one person's own directory, so a field of any
    // other kind here would be a value one machine decides for everybody.
    //
    // WHY IT IS A SECOND per-machine FILE RATHER THAN MORE ROWS ABOVE. `editor.json` is opened by the
    // Editor target and by nothing else; every field below is read by SceneRenderer, which the packaged
    // game also runs. One kind, two audiences, two files — and one SCHEMA, whose location is a parameter.
    //
    // The consumer named is SceneRenderer.cpp for the four the renderer reads per frame; MSAASamples is
    // read there too, at Init, and is separately spent by the panel that offers it.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kSceneRendererImpl = "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp";

    constexpr Row kMachineSettingsRows[] = {
         // Baked into every pipeline at SceneRenderer::Init, so it costs a restart — which is exactly the
         // property that makes it the machine's and not the level's.
         { "MSAASamples", Owner::Machine, kSceneRendererImpl },

         // The five К3 took out of the level file. Each passes the mis-authored/rendered-worse test on the
         // "rendered worse" side: MeshLOD off is byte-identical geometry near the camera, Anisotropy 1 and
         // Nearest filtering and AA None are the same picture blurrier or harsher, and CloudQuality High
         // reproduces the calibrated constants to the digit.
         { "AA", Owner::Machine, kSceneRendererImpl },
         { "MeshLOD", Owner::Machine, kSceneRendererImpl },
         { "TextureFilterMode", Owner::Machine, kSceneRendererImpl },
         { "Anisotropy", Owner::Machine, kSceneRendererImpl },
         { "CloudQualityTier", Owner::Machine, kSceneRendererImpl },
    };

    // ------------------------------------------------------------------------------------------------
    // <Name>.deproj — Common::Project::ProjectFile
    //
    // Four Project fields, one FileMeta, and one that is in the wrong file (EngineVersion).
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kProjectContext = "Desert/Desert/Source/Engine/Project/ProjectContext.cpp";

    // THE CONSUMER OF THREE OF THESE FIELDS LEFT THIS REPOSITORY (Л3). `FileVersion`, `Description` and
    // `EngineVersion` are written and read by the LAUNCHER, which now lives in `desert-launcher` — so
    // there is no file here to anchor a read in, and naming one would be a path that resolves to
    // nothing. Their rows therefore carry `Where = nullptr`, which this table already means literally:
    // "ownership is stated, readership is asserted elsewhere" (the SceneSettings rows use it for the
    // same reason and say so at the field's declaration).
    //
    // This is NOT a hole. It is the desert-shared arrangement applied a second time: each side asserts
    // its OWN end. The engine asserts that it does not write these fields; the launcher's own suite
    // asserts that it reads them. A census that named a file across a repository boundary would be
    // asserting something it cannot see, which is worse than asserting less.

    constexpr Row kProjectFileRows[] = {
         // The descriptor's own format generation, stamped by WriteProjectFile and by nothing else.
         { "FileVersion", Owner::FileMeta },

         { "Name", Owner::Project, kEditorLayer },
         // The load-bearing one: it remaps every content path into the project folder.
         { "AssetsRoot", Owner::Project, kProjectContext },
         { "DefaultScene", Owner::Project, kProjectContext },
         { "Description", Owner::Project },

         // K4, CLOSED: this is a Project fact now, not a Machine one. It used to be stamped with
         // Common::Version::Full() — this machine's commit hash and `.dirty` flag — on every
         // ProjectContext::Save(), i.e. whenever anybody picked a startup scene, into a file git tracks.
         // The engine no longer writes it at all; the launcher writes it once, at creation, and it means
         // THE ENGINE THIS PROJECT WAS CREATED WITH. That is stable, it is the same for everybody who
         // opens the project, and it is exactly what a .deproj is for.
         // The consumer is the launcher's project screen, which DRAWS it; Projects.cpp is where it is
         // written, at creation, and the census names readers.
         { "EngineVersion", Owner::Project },
    };

    // ------------------------------------------------------------------------------------------------
    // <Name>.desce, top level — Desert::Core::SceneSerialized
    // ------------------------------------------------------------------------------------------------

    constexpr Row kSceneFileRows[] = {
         { "SceneName", Owner::FileMeta },    // the name this file is filed under; also decides where a save lands
         { "Entities", Owner::Level },        // the world itself
         { "Settings", Owner::Level },        // the block censused below
         { "UnitVersion", Owner::FileMeta },  // world-unit generation
         { "SceneVersion", Owner::FileMeta }, // schema generation
    };

    // ------------------------------------------------------------------------------------------------
    // The Settings block of a .desce — Desert::Core::SceneSettings
    //
    // KIND ONLY. SettingConsumers owns "does anything read this" for every reflected type and answers it
    // for all 51 of these fields; repeating those answers here would be a second statement of one set.
    //
    // NOT ONE ROW SAYS MACHINE ANY MORE, and that is К3's deliverable: five did, and they are censused
    // under machine.json above. The four fields that LOOK like them and are not — RenderingPath,
    // GlobalIllumination, EnableSSAO, EnableSSR — are Level by the mis-authored / rendered-worse test at
    // the top of this file, and they stayed.
    // ------------------------------------------------------------------------------------------------

    constexpr Row kSceneSettingsRows[] = {
         { "RenderingPath", Owner::Level }, // two shading models, not two fidelities
         { "EnableSSAO", Owner::Level },    // the closest call; see the header
         { "GlobalIllumination", Owner::Level },
         { "GIIntensity", Owner::Level },
         { "EnableSSR", Owner::Level },
         { "SSRIntensity", Owner::Level },
         { "SSRMaxDistance", Owner::Level },

         { "EnableShadows", Owner::Level },
         { "ShadowBias", Owner::Level },
         { "CascadeSplitLambda", Owner::Level },

         // The grade. "Which one a scene is graded through is a property of the scene, the way film stock
         // was a property of the shoot" — SceneSettings.hpp's own words, and they are the rule's words.
         { "Tonemapper", Owner::Level },
         { "Exposure", Owner::Level },
         { "Gamma", Owner::Level },
         { "WhitePoint", Owner::Level },
         { "AutoExposure", Owner::Level },
         { "AutoExposureKey", Owner::Level },
         { "AutoExposureSpeed", Owner::Level },
         { "AutoExposureMin", Owner::Level },
         { "AutoExposureMax", Owner::Level },

         { "EnableBloom", Owner::Level },
         { "BloomThreshold", Owner::Level },
         { "BloomIntensity", Owner::Level },
         { "LensDispersion", Owner::Level },

         // The lens. Every one of these is authored content: the pass hardcodes no colour, no ghost count
         // and no spacing.
         { "EnableLensFlare", Owner::Level },
         { "LensFlareIntensity", Owner::Level },
         { "LensFlareTint", Owner::Level },
         { "LensFlareThreshold", Owner::Level },
         { "LensFlareGhostCount", Owner::Level },
         { "LensFlareGhostSpacing", Owner::Level },
         { "LensFlareGhostSizeNear", Owner::Level },
         { "LensFlareGhostSizeFar", Owner::Level },
         { "LensFlareGhostTintInner", Owner::Level },
         { "LensFlareGhostTintOuter", Owner::Level },
         { "LensFlareHaloIntensity", Owner::Level },
         { "LensFlareHaloRadius", Owner::Level },
         { "LensFlareStreakIntensity", Owner::Level },
         { "LensFlareStreakLength", Owner::Level },
         { "LensFlareStreakAngle", Owner::Level },
         { "LensFlareChromaShift", Owner::Level },

         { "Gravity", Owner::Level },
         // The three Wind rows left with their fields (Г26): they were level data with no reader, and a
         // value nobody reads has no owner to argue about.

         // The shipping player's splash for this level.
         { "SplashSprite", Owner::Level },
         { "SplashDuration", Owner::Level },
         { "SplashFade", Owner::Level },
    };

    constexpr FileCensus kFiles[] = {
         { "~/.desertengine/editor.json", Owner::Machine, "EditorPreferences", "EditorPreferences", "Get",
           CENSUS_ROWS( kEditorPrefsRows ) },
         { "~/.desertengine/editor.json (DebugView)", Owner::Machine, "DebugViewState", nullptr, nullptr,
           CENSUS_ROWS( kDebugViewRows ) },
         { "machine.json", Owner::Machine, "MachineSettings", "MachineSettings", "Get",
           CENSUS_ROWS( kMachineSettingsRows ) },
         { "<Name>.deproj", Owner::Project, "ProjectFile", "ProjectContext", "Current",
           CENSUS_ROWS( kProjectFileRows ) },
         { "<Name>.desce", Owner::Level, "SceneSerialized", nullptr, nullptr, CENSUS_ROWS( kSceneFileRows ) },
         { "<Name>.desce (Settings)", Owner::Level, "SceneSettings", nullptr, nullptr,
           CENSUS_ROWS( kSceneSettingsRows ) },
    };

    // ------------------------------------------------------------------------------------------------
    // THE DEBT REGISTER. Every field currently in the wrong file, and THE TASK THAT OWNS MOVING IT.
    //
    // A task name is mandatory and is checked for. An exception list without one is a list nobody can read
    // in a month, which is the state this whole subject was in before К1.
    //
    // Repairing one of these is a two-line edit here (delete the row, correct the Kind), and the corpus
    // test below then requires that the FILES were converted too — which is what makes the migration
    // provably run rather than merely written.
    // ------------------------------------------------------------------------------------------------

    struct Misplaced
    {
        const char* File;
        const char* Field;
        const char* Task; // must be non-empty; the suite checks
        const char* Why;
    };

    // std::array, НЕ C-массив: массива нулевой длины в C++ не существует, и MSVC отвергает его
    // (C2466), тогда как clang принимает как расширение GNU. Пустой реестр — то состояние, ради
    // которого файл и писался, поэтому тип обязан уметь его выразить: иначе одна строка осталась бы
    // здесь навсегда просто чтобы всё компилировалось. Ровно тот же случай был у PureVirtualCensus
    // в тот же день, и там он вскрылся локально; здесь — только на Windows CI.
    constexpr std::array<Misplaced, 0> kKnownMisplaced = {
         // THE REGISTER IS EMPTY, and an empty one is the only state this file is finished in. Every row
         // it has ever held was closed rather than reworded, which is deliberate: a register that keeps a
         // history is a register nobody reads.
         //
         // ---- К3: CLOSED. Five image-quality fields in the level file ------------------------------
         // AA, MeshLOD, TextureFilterMode, Anisotropy and CloudQualityTier are read by SceneRenderer,
         // which the packaged game also runs, so their destination was NOT editor.json — see the header.
         // They are in Common::Settings::MachineSettings, censused above, and scene schema v14 -> v15
         // strips them from every .desce (Migration::kRetiredKeys). The corpus assertion below is what
         // makes that provable rather than merely written: with the rows gone, it requires that the FILES
         // were converted too.
         //
         // ---- К4: CLOSED by К11, and the row is gone rather than reworded -------------------------
         // It read: ProjectContext::Save() stamps Common::Version::Full() — the commit hash and a
         // `.dirty` suffix — into a git-tracked file, triggered by the Build Settings startup-scene
         // combo, with zero functional readers (the compatibility check its own header cited runs
         // against Common::Version::CommitCount()).
         //
         // The engine no longer writes the field. It is now written once, by the launcher, at creation,
         // and means THE ENGINE THE PROJECT WAS CREATED WITH — a Project fact, stable, shared, and
         // correctly placed by the §6 procedure. It is a normal Project row above. Deleting the field
         // instead was considered and refused: it is a real answer to "which version was this made in",
         // and reinstating it later costs more than keeping it true.
    };

    // ------------------------------------------------------------------------------------------------
    // Reading the tree
    // ------------------------------------------------------------------------------------------------

    // Walks up from the working directory looking for a file only the repository has — the test runner's
    // working directory is not fixed. Same shape as SceneDebugFields and SceneVersionGate.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The field names rfl::json::write will emit for T — i.e. the keys of the file, asked of the same
    // library that writes them. A hand-typed list here would be a third statement of the format.
    //
    // ONE MEMBER KIND IS NOT A KEY, AND rfl::fields<> DOES NOT KNOW THAT. An rfl::ExtraFields member is
    // SPREAD at the struct's own level by the writer instead of being nested under its member name
    // (NamedTupleParser::add_field_to_object), so it contributes whatever keys it holds and never one
    // called after itself. EditorPreferences::UnknownKeys is one — К9 added it so that a build saving
    // editor.json stops deleting the keys it does not know — and counting it here would demand a census
    // row for a key the file can never contain.
    //
    // The skip is decided by rfl::internal::is_extra_fields_v, the SAME trait the writer branches on,
    // rather than by the member's name, so it cannot drift into an exemption for something else. That it
    // still agrees with an actual rfl::json::write is asserted in Desert/Tests/Editor/PreferenceOwnership
    // (`TheKeysWrittenAreExactlyTheStructsFieldsPlusThePreservedOnes`) — this suite reads a field list and
    // that one reads the bytes, which is the pair that keeps a census honest.
    template <class NamedTupleType, std::size_t I>
    void AppendSerializedKeys( std::vector<std::string>& names )
    {
        if constexpr ( I < NamedTupleType::size() )
        {
            using FieldType = rfl::tuple_element_t<I, typename NamedTupleType::Fields>;
            if constexpr ( !rfl::internal::is_extra_fields_v<typename FieldType::Type> )
                names.push_back( std::string( FieldType::name() ) );
            AppendSerializedKeys<NamedTupleType, I + 1>( names );
        }
    }

    template <class T>
    std::vector<std::string> SerializedKeysOf()
    {
        std::vector<std::string> names;
        AppendSerializedKeys<rfl::named_tuple_t<T>, 0>( names );
        return names;
    }

    std::vector<std::string> ReflectedKeysOf( const char* typeName )
    {
        std::vector<std::string> names;
        const auto*              info = ReflectionRegistry::Get().Find( typeName );
        if ( info == nullptr )
            return names;
        for ( const FieldInfo& field : info->Fields )
            names.push_back( field.Name );
        return names;
    }

    // The keys of one census's file, from that file's own writer.
    std::vector<std::string> KeysOfFile( const FileCensus& file )
    {
        const std::string type = file.Type;
        if ( type == "EditorPreferences" )
            return SerializedKeysOf<Desert::Editor::EditorPreferences>();
        if ( type == "DebugViewState" )
            return SerializedKeysOf<Desert::Graphic::DebugViewState>();
        if ( type == "MachineSettings" )
            return SerializedKeysOf<Common::Settings::MachineSettings>();
        if ( type == "ProjectFile" )
            return SerializedKeysOf<Common::Project::ProjectFile>();
        if ( type == "SceneSerialized" )
            return SerializedKeysOf<Desert::Core::SceneSerialized>();
        if ( type == "SceneSettings" )
            return ReflectedKeysOf( "SceneSettings" );
        return {};
    }

    // ------------------------------------------------------------------------------------------------
    // The consumer half — two receiver shapes on top of SettingConsumers' reader
    // ------------------------------------------------------------------------------------------------

    // `std::optional<ProjectFile> s_Current;` — the anchor is a template ARGUMENT, so the declaration
    // shape DeriveReceivers recognises (`Type name`) does not fire and the binding is invisible to it.
    // Without this, ProjectContext.cpp's `s_Current->AssetsRoot` reads as unanchored and the load-bearing
    // consumer of the .deproj looks like it reads nothing.
    std::vector<std::string> TemplateArgumentReceivers( const std::string& text, const std::string& anchor )
    {
        using namespace Desert::Tests::ConsumerText;

        std::vector<std::string> out;
        for ( std::size_t at : WordPositions( text, anchor ) )
        {
            std::size_t i = SkipSpace( text, at + anchor.size() );
            if ( i >= text.size() || text[i] != '>' )
                continue;
            i = SkipSpace( text, i + 1 );
            while ( i < text.size() && ( text[i] == '&' || text[i] == '*' ) )
                i = SkipSpace( text, i + 1 );
            const std::string name = IdentAt( text, i );
            if ( !name.empty() )
                out.push_back( name );
        }
        return out;
    }

    // The identifier immediately before the `::` that qualifies position `at`, or "" when `at` is
    // unqualified. `Desert::Editor::EditorPreferences::Get` answers "EditorPreferences" for the `Get`.
    std::string QualifierBefore( const std::string& text, std::size_t at )
    {
        using namespace Desert::Tests::ConsumerText;

        std::size_t i = at;
        while ( i > 0 && std::isspace( static_cast<unsigned char>( text[i - 1] ) ) != 0 )
            --i;
        if ( i < 2 || text[i - 1] != ':' || text[i - 2] != ':' )
            return {};
        i -= 2;
        while ( i > 0 && std::isspace( static_cast<unsigned char>( text[i - 1] ) ) != 0 )
            --i;
        const std::size_t end = i;
        while ( i > 0 && IsIdentChar( text[i - 1] ) )
            --i;
        return text.substr( i, end - i );
    }

    // A CALL of `name`, optionally qualified by `qualifier` — `MachineSettings::Load(`, `GameUserDirectory(`.
    //
    // IT HAS TO BE A CALL AND NOT A MENTION, and that is not pedantry: the first version of the assertion
    // below asked only whether the WORDS appeared, and deleting the whole `MachineSettings::Load(...)`
    // statement from Runtime/Source/Main.cpp left it green — the `#include <Common/Settings/
    // MachineSettings.hpp>` line is not a string literal, so the stripper keeps it, and `Load` is a common
    // word. A check that cannot fail is worth exactly what an absent one is; the mutation is what found it.
    bool CallsFunction( const std::string& text, const std::string& qualifier, const std::string& name )
    {
        using namespace Desert::Tests::ConsumerText;

        for ( std::size_t at : WordPositions( text, name ) )
        {
            if ( !qualifier.empty() && QualifierBefore( text, at ) != qualifier )
                continue;
            const std::size_t i = SkipSpace( text, at + name.size() );
            if ( i < text.size() && text[i] == '(' )
                return true;
        }
        return false;
    }

    // `ProjectContext::Current().Name`, `EditorPreferences::Get().DebugView`, and — inside the type's own
    // implementation file — the bare `Get().FavouriteFields`. A static accessor handing the value out with
    // no local in between; AnchorReadsField stops at the `::`, so the chain is walked here.
    //
    // THE BARE FORM IS ONLY ACCEPTED IN A FILE THAT DEFINES MEMBERS OF THE TYPE, and that restriction is
    // what keeps it from being vacuous: `Get` is the commonest accessor name in this engine, so crediting
    // every unqualified `Get().Something` everywhere would let any singleton's field certify this one.
    // Inside EditorPreferences.cpp an unqualified `Get()` is this type's by the language's own rules.
    bool AccessorReadsField( const std::string& text, const std::string& holder, const std::string& accessor,
                             const std::string& field )
    {
        using namespace Desert::Tests::ConsumerText;

        // Does this file define members of the type at all? (`Holder::something` appearing anywhere.)
        bool definesMembers = false;
        for ( std::size_t at : WordPositions( text, holder ) )
        {
            const std::size_t i = SkipSpace( text, at + holder.size() );
            definesMembers = definesMembers || ( i + 1 < text.size() && text[i] == ':' && text[i + 1] == ':' );
        }

        for ( std::size_t at : WordPositions( text, accessor ) )
        {
            const std::string qualifier = QualifierBefore( text, at );
            const bool        reachable = qualifier == holder || ( qualifier.empty() && definesMembers );
            if ( !reachable )
                continue;

            std::size_t i = SkipSpace( text, at + accessor.size() );
            if ( i >= text.size() || text[i] != '(' )
                continue;

            int depth = 0;
            while ( i < text.size() )
            {
                if ( text[i] == '(' )
                    ++depth;
                else if ( text[i] == ')' && --depth == 0 )
                {
                    ++i;
                    break;
                }
                ++i;
            }

            if ( MemberReadAt( text, i, field ) )
                return true;
        }
        return false;
    }

    bool FileReadsField( const std::string& text, const FileCensus& census, const std::string& field )
    {
        using namespace Desert::Tests::ConsumerText;

        std::vector<std::string> receivers = DeriveReceivers( text, { census.Type } );
        for ( const std::string& extra : TemplateArgumentReceivers( text, census.Type ) )
            receivers.push_back( extra );

        for ( const std::string& receiver : receivers )
            if ( ReceiverReadsField( text, receiver, field ) )
                return true;

        if ( AnchorReadsField( text, census.Type, field ) )
            return true;

        if ( census.Holder != nullptr && census.Accessor != nullptr )
            return AccessorReadsField( text, census.Holder, census.Accessor, field );

        return false;
    }

    bool IsKnownMisplaced( const char* file, const std::string& field )
    {
        for ( const Misplaced& m : kKnownMisplaced )
            if ( field == m.Field && std::string( file ) == m.File )
                return true;
        return false;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 0. THE SUITE CAN SEE WHAT IT CLAIMS TO CHECK
// ---------------------------------------------------------------------------------------------------

// Without this, every loop below runs over an empty set and reports green — the failure mode that looks
// exactly like a census that found nothing wrong. SceneDebugFields learned the same lesson; so did
// SceneVersionGate, which pins its corpus size for the same reason.
TEST( ConfigOwnership, TheSourcesThisSuiteReadsAreWhereItThinksTheyAre )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "repository root not found from the test's working directory";

    for ( const FileCensus& file : kFiles )
    {
        SCOPED_TRACE( file.File );
        EXPECT_FALSE( KeysOfFile( file ).empty() )
             << file.Type
             << " enumerated to no keys at all - the struct moved, was renamed, or is no "
                "longer serialized the way this suite asks about it";
    }

    // The reflected block is the one that comes from a GENERATED table, so it is the one that can be
    // silently absent when the header tool has not run.
    EXPECT_NE( ReflectionRegistry::Get().Find( "SceneSettings" ), nullptr )
         << "SceneSettings is not reflected - the generated table is stale or was not compiled in";
}

// ---------------------------------------------------------------------------------------------------
// 1. THE CENSUS IS COMPLETE, IN BOTH DIRECTIONS
// ---------------------------------------------------------------------------------------------------

// A field added to any of the three files tomorrow lands here first, and the person adding it has to say
// which kind it is. That decision is the entire point of this suite; everything else follows from it.
TEST( ConfigOwnership, EveryKeyOfEveryConfigFileIsCensusedExactlyOnce )
{
    for ( const FileCensus& file : kFiles )
    {
        SCOPED_TRACE( file.File );

        std::vector<std::string> fromWriter = KeysOfFile( file );
        std::vector<std::string> fromTable;
        for ( const Row* r = file.Rows; r != file.Rows + file.Count; ++r )
            fromTable.push_back( r->Field );

        // A duplicated row needs no assertion of its own: it makes this vector one longer than the
        // writer's and the comparison below says so, naming the repeated field.
        std::sort( fromWriter.begin(), fromWriter.end() );
        std::sort( fromTable.begin(), fromTable.end() );

        EXPECT_EQ( fromWriter, fromTable )
             << "the census of " << file.File
             << " and what actually gets written to it disagree. A key "
                "the writer emits and this table does not name is a setting nobody has decided the owner "
                "of; a row naming a key that is no longer written is a stale row.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. THE RULE ITSELF
// ---------------------------------------------------------------------------------------------------

// One file, one kind. FileMeta is additionally allowed everywhere because it is not a setting at all — it
// is the file describing itself, and it belongs to whichever file it describes by definition.
TEST( ConfigOwnership, EveryFieldIsOfItsOwnFilesKind )
{
    for ( const FileCensus& file : kFiles )
    {
        SCOPED_TRACE( file.File );
        for ( const Row* r = file.Rows; r != file.Rows + file.Count; ++r )
        {
            if ( r->Kind == Owner::FileMeta || r->Kind == file.Holds )
                continue;

            EXPECT_TRUE( IsKnownMisplaced( file.File, r->Field ) )
                 << file.File << " holds " << r->Field << ", which is " << OwnerName( r->Kind ) << " and not "
                 << OwnerName( file.Holds )
                 << ". Either the row's kind is wrong, or the field is in the wrong file and needs a "
                    "kKnownMisplaced entry naming the task that will move it.";
        }
    }
}

// FileMeta is the one kind that can be claimed to dodge the rule, so the set that may claim it is pinned
// exactly rather than left to judgement. A version, a unit generation and the name a file is filed under
// are the whole of it; a sixth field calling itself metadata is somebody widening a loophole.
TEST( ConfigOwnership, OnlyFourFieldsAreFileMetadataAndTheseAreThey )
{
    std::vector<std::string> meta;
    for ( const FileCensus& file : kFiles )
        for ( const Row* r = file.Rows; r != file.Rows + file.Count; ++r )
            if ( r->Kind == Owner::FileMeta )
                meta.push_back( std::string( file.File ) + "::" + r->Field );

    std::sort( meta.begin(), meta.end() );

    const std::vector<std::string> expected = {
         "<Name>.deproj::FileVersion",
         "<Name>.desce::SceneName",
         "<Name>.desce::SceneVersion",
         "<Name>.desce::UnitVersion",
    };

    EXPECT_EQ( meta, expected ) << "the FileMeta kind is the rule's one exemption, and this is the list of "
                                   "everything allowed to claim it. A new entry needs an argument, not a row.";
}

// ---------------------------------------------------------------------------------------------------
// 3. THE DEBT REGISTER, PINNED IN BOTH DIRECTIONS
// ---------------------------------------------------------------------------------------------------

// Stated as an exact set rather than a count, for the reason SettingConsumers' dead-setting register gives:
// a count lets repairing one violation and introducing another cancel out, which is precisely how a
// register stops being read.
TEST( ConfigOwnership, TheKnownMisplacedFieldsAreExactlyThese )
{
    std::vector<std::string> fromTables;
    for ( const FileCensus& file : kFiles )
        for ( const Row* r = file.Rows; r != file.Rows + file.Count; ++r )
            if ( r->Kind != Owner::FileMeta && r->Kind != file.Holds )
                fromTables.push_back( std::string( file.File ) + "::" + r->Field );

    std::vector<std::string> registered;
    for ( const Misplaced& m : kKnownMisplaced )
        registered.push_back( std::string( m.File ) + "::" + m.Field );

    std::sort( fromTables.begin(), fromTables.end() );
    std::sort( registered.begin(), registered.end() );

    EXPECT_EQ( fromTables, registered )
         << "a field whose kind does not match its file is not in the debt register, or the register names "
            "one that has since been moved. Both are edits somebody has to see.";

    // NONE TODAY. It was six until К11 closed К4 (`.deproj::EngineVersion` stopped being a machine fact
    // when the engine stopped stamping it) and five until К3 moved the quality group out of the level
    // file. Zero is not a milestone to be defended — a new violation is allowed to appear here with a
    // task name — but it does mean that every file in this repository now states only its own kind, and
    // that is the first time that has been true. This number going UP without a task name is what the
    // next assertion refuses.
    EXPECT_EQ( registered.size(), 0u );
}

// A debt entry with no owner is a note, and a note nobody owns is what this whole subject was made of
// before К1. The task name is what turns the list into work.
TEST( ConfigOwnership, EveryDebtEntryNamesTheTaskThatOwnsIt )
{
    for ( const Misplaced& m : kKnownMisplaced )
    {
        SCOPED_TRACE( std::string( m.File ) + "::" + m.Field );
        ASSERT_NE( m.Task, nullptr );
        EXPECT_GE( std::string( m.Task ).size(), 2u )
             << "an exception without a task name is unreadable in a month - name the task that moves it";
        ASSERT_NE( m.Why, nullptr );
        EXPECT_GE( std::string( m.Why ).size(), 20u ) << "say what the field is and why it is misplaced";
    }
}

// ---------------------------------------------------------------------------------------------------
// 4. NO VALUE IS STATED BY TWO FILES
// ---------------------------------------------------------------------------------------------------

// The other half of one-source-of-truth (contract §2.1): the rule above stops a field landing in the wrong
// file, and this stops it landing in two. A duplicated setting does not fail anything at first — it fails
// the day the two copies disagree, which is a desync bug with no error message.
TEST( ConfigOwnership, NoSettingIsStatedByTwoDifferentFiles )
{
    for ( std::size_t a = 0; a < std::size( kFiles ); ++a )
    {
        for ( std::size_t b = a + 1; b < std::size( kFiles ); ++b )
        {
            for ( const Row* x = kFiles[a].Rows; x != kFiles[a].Rows + kFiles[a].Count; ++x )
            {
                for ( const Row* y = kFiles[b].Rows; y != kFiles[b].Rows + kFiles[b].Count; ++y )
                {
                    if ( std::string( x->Field ) != y->Field )
                        continue;

                    // The two halves of editor.json are one file, and a name repeated between the outer
                    // struct and its nested block would be a real collision - so this pair is NOT exempt.
                    ADD_FAILURE() << x->Field << " is written by BOTH " << kFiles[a].File << " and "
                                  << kFiles[b].File
                                  << ". Two stores for one value is a desync waiting for the day they "
                                     "disagree; delete one and give the survivor a reader.";
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// 5. THE CONSUMER HALF, for the two files no other census covers
// ---------------------------------------------------------------------------------------------------

// SettingConsumers covers every REFLECTED type. editor.json and .deproj are not reflected — they go
// through rfl directly — so until К1 nothing asked whether their fields reach anything, and the first
// look found two settings in editor.json that no line of code outside their own declaration mentioned
// (PhotogrammetryCaptureCommand and PhotogrammetryMode; both deleted by К1).
//
// The check is deliberately conservative in one known direction: a field whose only consumer sits in the
// same file as the widget that edits it cannot be told apart from the widget's own `&prefs.Field` pointer.
// That does not weaken what this catches, which is the failure that actually happened - a field NOTHING
// anywhere mentions.
TEST( ConfigOwnership, EveryFieldOfTheUnreflectedFilesNamesAConsumerThatReadsIt )
{
    using namespace Desert::Tests::ConsumerText;

    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const FileCensus& file : kFiles )
    {
        for ( const Row* r = file.Rows; r != file.Rows + file.Count; ++r )
        {
            if ( r->Where == nullptr )
                continue;

            SCOPED_TRACE( std::string( file.File ) + "::" + r->Field );

            const std::string text = StripCommentsAndLiterals( ReadAll( root + r->Where ) );
            ASSERT_FALSE( text.empty() ) << "named consumer " << r->Where << " could not be read";

            EXPECT_TRUE( FileReadsField( text, file, r->Field ) )
                 << r->Where << " is named as the consumer of " << r->Field
                 << " but contains no read of that field on a value of type " << file.Type
                 << ". Either the read was removed and the setting is now dead, or it moved to another "
                    "file and this row must follow it.";
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// 6. THE CORPUS — the half that proves a migration was RUN and not merely written
// ---------------------------------------------------------------------------------------------------

// A .desce may state Level keys and its own metadata, and nothing else. It used to also state the five К3
// owed, exempted BY NAME through the register rather than by loosening the check — so deleting those rows
// is what turned this assertion into a requirement that the FILES were converted, which is the only thing
// that ever makes a migration expire. It fires today over an empty register, which is the state it was
// built to reach.
//
// A key here is forbidden by KIND, not by name, so it also covers a key that no longer exists as a field
// at all: the loop below asks the .desce census for everything that is not this file's kind, and a Machine
// row added to it tomorrow reddens every scene that states it without anybody editing this test.
TEST( ConfigOwnershipCorpus, NoSceneOnDiskStatesASettingOfAnotherFilesKind )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // WHAT A .desce MAY NOT STATE, derived from the OTHER censuses rather than from this one — and the
    // change of direction is К3's, because the old derivation stopped saying anything the moment it
    // worked. It read the .desce census for rows whose kind was not Level, which was exactly right while
    // five such rows sat there under a debt exemption; with the rows moved, that set is EMPTY and the
    // loop below would have swept fifty-one files against nothing and reported green. A container whose
    // contents come from the same source as the question is §1.4's shape, and it arrived here as the
    // SUCCESS case of a repair.
    //
    // So the set is every key some OTHER file owns: a scene may not state a machine's quality, a user's
    // outline colour or the project's assets root. It is non-empty by construction — the censuses above
    // are pinned against their own writers — and a key added to any of them extends this guard for free.
    std::vector<std::string> forbidden;
    for ( const FileCensus& file : kFiles )
    {
        const bool isSceneFile =
             std::string( file.File ) == "<Name>.desce (Settings)" || std::string( file.File ) == "<Name>.desce";
        for ( const Row* r = file.Rows; r != file.Rows + file.Count; ++r )
        {
            if ( r->Kind == Owner::FileMeta )
                continue; // a version integer is the file's own and collides with nothing
            if ( isSceneFile )
            {
                // A row still in the scene census that is not Level is a live debt entry; it stays
                // exempt until the task that owns it converts the files.
                if ( r->Kind != file.Holds && !IsKnownMisplaced( file.File, r->Field ) )
                    forbidden.push_back( r->Field );
                continue;
            }
            forbidden.push_back( r->Field );
        }
    }
    ASSERT_FALSE( forbidden.empty() ) << "nothing is forbidden, so this sweep proves nothing";

    std::vector<std::filesystem::path> scenes;
    std::error_code                    ec;
    for ( const auto& entry :
          std::filesystem::recursive_directory_iterator( root + "Editor/Resources/Assets/Scenes", ec ) )
        if ( entry.is_regular_file() && entry.path().extension() == ".desce" )
            scenes.push_back( entry.path() );

    ASSERT_GE( scenes.size(), 40u ) << "the scene corpus was not found";

    for ( const auto& path : scenes )
    {
        const auto parsed = rfl::json::read<rfl::Generic>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string() << " is not readable JSON";
        const auto root_object = parsed.value().to_object();
        ASSERT_TRUE( root_object.has_value() ) << path.string() << " is not a JSON object";

        const auto settings = root_object.value().get( "Settings" );
        if ( !settings.has_value() )
            continue;
        const auto fields = settings.value().to_object();
        if ( !fields.has_value() )
            continue;

        for ( const std::string& key : forbidden )
            EXPECT_FALSE( fields.value().get( key ).has_value() )
                 << path.string() << " states Settings." << key
                 << ", which another config file owns. Run Tools/SceneMigrator over it.";
    }
}

// The `.deproj` this repository ships is tracked by git, and it states no machine-specific key.
//
// The event it was built to catch has been removed rather than merely watched: ProjectContext::Save()
// used to stamp EngineVersion with this machine's commit hash (and `.dirty`) on every write, triggered by
// Build Settings -> Startup scene, and К11 stopped it. The tripwire stays because the census is what makes
// it general — any FUTURE field declared Machine in the .deproj census reddens this the moment the tracked
// descriptor states it, and that is worth keeping whether or not a Machine row exists today.
TEST( ConfigOwnershipCorpus, TheTrackedProjectDescriptorStatesNoMachineSpecificKey )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string text = ReadAll( root + "Editor/Desert.deproj" );
    ASSERT_FALSE( text.empty() ) << "Editor/Desert.deproj is missing or empty";

    const auto parsed = rfl::json::read<rfl::Generic>( text );
    ASSERT_TRUE( parsed.has_value() ) << "Editor/Desert.deproj is not readable JSON";
    const auto object = parsed.value().to_object();
    ASSERT_TRUE( object.has_value() );

    for ( const FileCensus& file : kFiles )
    {
        if ( std::string( file.File ) != "<Name>.deproj" )
            continue;
        for ( const Row* r = file.Rows; r != file.Rows + file.Count; ++r )
        {
            if ( r->Kind != Owner::Machine )
                continue;
            EXPECT_FALSE( object.value().get( r->Field ).has_value() )
                 << "Editor/Desert.deproj now states " << r->Field
                 << ", a per-machine value, in a file git tracks and the whole team shares. See К4.";
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// 7. THE RELATION К3 IS ABOUT: TURNING THE MACHINE DOWN WRITES NOTHING INTO A LEVEL
// ---------------------------------------------------------------------------------------------------

// The whole point of the move, stated as a fact about BYTES rather than about where a field is declared.
// Before К3 a person on a weak machine who picked Low clouds and 1x anisotropy had, by that act, modified
// the level file — and pushed it, which is how `ShowColliders: true` reached 55 of 73 scenes before К2.
//
// It is deliberately end-to-end and not a fact about types: the quality is set to its cheapest value on
// every field, the store is SAVED for real, and the corpus is hashed on both sides of that write. A
// future SceneSerializer that learned to mirror one of these values into the settings block would fail
// here, and nothing about the declarations would have changed.
//
// HOME IS NOT TOUCHED: the store is written to a temp file this test owns, through the path parameter
// that exists precisely so a caller can say where. That parameter is why this assertion is runnable at
// all — a store that computed its own location could only be tested against the developer's own config.
TEST( ConfigOwnershipCorpus, LoweringTheQualityOnThisMachineChangesNoByteOfAnySceneFile )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::filesystem::path> scenes;
    std::error_code                    ec;
    for ( const auto& entry :
          std::filesystem::recursive_directory_iterator( root + "Editor/Resources/Assets/Scenes", ec ) )
        if ( entry.is_regular_file() && entry.path().extension() == ".desce" )
            scenes.push_back( entry.path() );
    ASSERT_GE( scenes.size(), 40u ) << "the scene corpus was not found";

    std::vector<std::string> before;
    for ( const auto& path : scenes )
        before.push_back( ReadAll( path ) );

    const std::filesystem::path store =
         std::filesystem::temp_directory_path() / "desert_configownership_machine.json";
    std::filesystem::remove( store, ec );
    Common::Settings::MachineSettings::Load( store );

    auto& quality             = Common::Settings::MachineSettings::Get();
    quality.MSAASamples       = 1;
    quality.AA                = Common::Settings::AntiAliasingMode::None;
    quality.MeshLOD           = false;
    quality.TextureFilterMode = Common::Settings::TextureFilter::Nearest;
    quality.Anisotropy        = 1;
    quality.CloudQualityTier  = Common::Settings::CloudQuality::Low;
    ASSERT_TRUE( Common::Settings::MachineSettings::Save() );
    ASSERT_TRUE( std::filesystem::exists( store ) ) << "the quality was not written anywhere at all";

    for ( std::size_t i = 0; i < scenes.size(); ++i )
        EXPECT_EQ( ReadAll( scenes[i] ), before[i] )
             << scenes[i].string() << " changed when the MACHINE's quality did";

    std::filesystem::remove( store, ec );
}

// The same claim one layer in, through the call the saver actually writes the Settings block with. The
// test above proves nothing was written; this proves nothing WOULD be — the serialized block is a
// function of Core::SceneSettings alone, so a mirror added between the two structs shows up here as a
// difference rather than as a slow desync somebody notices months later.
TEST( ConfigOwnership, TheSerializedSettingsBlockDoesNotMoveWhenTheMachineQualityDoes )
{
    const auto* type = ReflectionRegistry::Get().Find( "SceneSettings" );
    ASSERT_NE( type, nullptr );

    const Desert::Core::SceneSettings settings;
    const std::string before = rfl::json::write( Desert::Reflection::SerializeReflected( *type, &settings ) );

    auto& quality             = Common::Settings::MachineSettings::Get();
    quality.MSAASamples       = 8;
    quality.AA                = Common::Settings::AntiAliasingMode::SMAA;
    quality.MeshLOD           = false;
    quality.TextureFilterMode = Common::Settings::TextureFilter::Nearest;
    quality.Anisotropy        = 16;
    quality.CloudQualityTier  = Common::Settings::CloudQuality::Low;

    EXPECT_EQ( rfl::json::write( Desert::Reflection::SerializeReflected( *type, &settings ) ), before );
}

// ---------------------------------------------------------------------------------------------------
// 8. BOTH HOSTS OPEN IT — which is the only thing that makes it a different file from editor.json
// ---------------------------------------------------------------------------------------------------

// The argument for a second per-user file is one sentence — the packaged game reads these values and
// never opens editor.json — and an argument is not a wiring.
//
// THE FAILURE THIS CLOSES ALREADY HAPPENED ONCE, from the target nobody looks at. `MSAASamples` was a
// per-machine field in editor.json, correctly typed and correctly censused, pushed into the renderer by
// EditorPreferences — which the Runtime does not link. So a shipped game ran at whatever the struct's
// default said, for ever, with no reader, no dial and no error anywhere. Nothing here could see it,
// because every census asked about the field and none asked about the HOST.
//
// So both ends are asserted rather than argued: the editor opens the store beside editor.json, the game
// opens it inside the player's own per-product directory, and the game hands the values to its renderer.
// A text census like the consumer half above; the alternative is launching a fullscreen game in CI.
TEST( ConfigOwnership, BothHostsOpenTheMachineStoreAndTheGameOpensItsOwnDirectory )
{
    using namespace Desert::Tests::ConsumerText;

    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    struct Host
    {
        const char* File;
        const char* Directory; // the call that says WHERE this host's copy lives
        const char* Why;
    };
    const Host hosts[] = {
         { "Editor/Source/EditorLayer.cpp", "ConfigDirectory",
           "the editor keeps its copy in ~/.desertengine, beside editor.json" },
         { "Runtime/Source/Main.cpp", "GameUserDirectory",
           "a packaged game keeps its copy in the player's own per-product directory, because a shipped "
           "game has no engine installation to belong to" },
    };

    for ( const Host& host : hosts )
    {
        SCOPED_TRACE( host.File );
        const std::string text = StripCommentsAndLiterals( ReadAll( root + host.File ) );
        ASSERT_FALSE( text.empty() ) << "could not read " << host.File;

        EXPECT_TRUE( CallsFunction( text, "MachineSettings", "Load" ) )
             << host.File
             << " never LOADS the machine store, so this host runs at the schema defaults "
                "whatever its user chose — "
             << host.Why;
        EXPECT_TRUE( CallsFunction( text, {}, host.Directory ) )
             << host.File << " does not call " << host.Directory << "(): " << host.Why;
    }

    // And the game hands them on, or the file is read and thrown away. The editor's own push is covered
    // by the consumer census above (EditorLayer is named for DebugView and reaches the same renderer).
    const std::string layer = StripCommentsAndLiterals( ReadAll( root + "Runtime/Source/RuntimeLayer.cpp" ) );
    ASSERT_FALSE( layer.empty() );
    EXPECT_TRUE( CallsFunction( layer, {}, "SetQuality" ) )
         << "the packaged game loads the machine's quality and never gives it to a renderer";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

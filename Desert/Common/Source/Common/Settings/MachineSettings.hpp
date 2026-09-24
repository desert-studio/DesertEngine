#pragma once

#include <filesystem>
#include <string>
#include <vector>

// The unknown-key carrier below is a FIELD of the struct, so rfl has to be visible here — the same
// reason Editor::EditorPreferences and Assets::EntityData declare theirs inline.
#include <rflcpp/rfl/ExtraFields.hpp>
#include <rflcpp/rfl/Generic.hpp>

namespace Common::Settings
{
    // WHAT THIS FILE IS, in one sentence: ONE HOST'S COPY OF WHAT ITS MACHINE CAN AFFORD.
    //
    // К1 wrote down three files and the question that decides between them (Desert/Tests/Engine/
    // ConfigOwnership). Its first question is "would two people working on this project at the same time
    // legitimately want different values?", and for a quality knob the answer is obviously yes — which
    // sent five of them to `editor.json` on paper and left them in the LEVEL file in fact, because
    // `editor.json` is an Editor-target concept the packaged Runtime never opens. Putting them there
    // would have fixed our git history by taking the quality dial away from the player.
    //
    // So the store is here, in Common, which both hosts link. ONE SCHEMA, TWO PLACES — and the place is a
    // PARAMETER of Load(), not a second type. The alternative that was refused (a per-host file with its
    // own struct) is the two-sides-must-agree shape this project has paid for five times in one week; a
    // parameter cannot drift from itself.
    //
    // THE TWO PLACES:
    //   * the editor   — `~/.desertengine/machine.json`, beside `editor.json`. Two different files on
    //                    purpose: `editor.json` is one person's copy of THE EDITOR (gizmo snap, panel
    //                    layout, packaging paths) and the shipped game has no use for any of it, while
    //                    every field below is read by SceneRenderer, which the shipped game runs.
    //   * the game     — `<user data>/<product>/machine.json`, the per-user writable directory a player's
    //                    save games belong in (GameUserDirectory below). Per PRODUCT, because two games
    //                    on one machine are two different budgets.
    //
    // WHAT IS IN IT AND WHAT IS NOT. The test is К1's and it is checkable rather than a matter of taste:
    // set the field to its CHEAPEST value — has the level been MIS-AUTHORED, or merely RENDERED WORSE?
    //   * rendered worse -> here. MeshLOD off is byte-identical geometry near the camera; Anisotropy 1,
    //     Nearest filtering, AA None and MSAA off are the same picture, blurrier or harsher; CloudQuality
    //     High reproduces the calibrated constants to the digit.
    //   * mis-authored -> the level file. Forward and Deferred disagree about cloud shadow on the ground;
    //     ACES and Reinhard are two grades; GI Off is a darker room, not a coarser one. Those stay in
    //     Core::SceneSettings and are named there.
    //
    // IT IS NOT REFLECTED AND CANNOT REACH A .desce. There is no REFLECT()/PROPERTY() here, so the
    // reflection serializer SceneSerializer writes the settings block with cannot see these fields —
    // the same construction Graphic::DebugViewState uses, and the reason К2's ten flags cannot come
    // back. Desert/Tests/Engine/ConfigOwnership asserts the relation directly: turning the machine down
    // does not change a byte of any scene file.

    // Post-process anti-aliasing. Deliberately no MSAA entry: MSAA is the sample count of the
    // framebuffer (MSAASamples below), it resolves geometry edges inside the pipeline, and the post
    // filters the resolved image. Both can be on at once — they are not alternatives, and the combo that
    // pretended they were is what made one user action write two different stores.
    enum class AntiAliasingMode : int
    {
        None = 0,
        FXAA,
        SMAA,
    };

    // Global texture sampler filter. Live: SceneRenderer pushes it into Graphic::RenderConfig and the
    // samplers are recreated on a change, so it applies without a reload.
    //
    // THIS ENUM USED TO EXIST TWICE — `Core::TextureFilter` in the scene settings and
    // `Graphic::TextureFilterMode` in RenderConfig, with a comment on the first saying "Must match
    // Graphic::TextureFilterMode" and nothing anywhere asserting that it did. A mirror with a comment
    // instead of a test is the defect shape this project keeps finding; both spellings are gone and this
    // is the only declaration.
    enum class TextureFilter : int
    {
        Nearest     = 0, // nearest min/mag + nearest mip
        Bilinear    = 1, // linear min/mag + nearest mip
        Trilinear   = 2, // linear min/mag + linear mip
        Anisotropic = 3, // trilinear + anisotropic filtering (if the device supports it)
    };

    // How much the volumetric cloud layer may spend on OCCLUSION — the shadow ray each lit sample traces
    // toward the sun, and the map the layer casts onto the world under it. See Graphic::CloudQualityScale
    // for what each tier changes and Docs/Clouds/CALIBRATION.md section QT for what each one costs.
    //
    //  High   — the reference. Everything at the value the calibration converged on.
    //  Medium — the cloud shadow map at HALF its linear footprint; the SKY is unchanged.
    //  Low    — the above, plus the shadow ray capped at 16 samples, which runs the sunward highlights
    //           about a third bright.
    enum class CloudQuality : int
    {
        Low    = 0,
        Medium = 1,
        High   = 2,
    };

    struct MachineSettings
    {
        // MSAA sample count for the scene's composite framebuffer (1 = off, 2/4/8). Read ONCE, at
        // SceneRenderer::Init, because the pipelines bake their sample count — so a change applies at the
        // next start and the editor says so. Clamped there to the device's own ceiling
        // (Graphic::RenderConfig::MaxMSAASamples).
        //
        // IT CAME FROM editor.json, AND THAT MOVE IS HALF OF WHY THIS FILE EXISTS. It was per-machine and
        // correctly so, but it was per-machine in a file only the EDITOR opens: the packaged game ran
        // MSAA nailed to 1 with no reader and no dial. ConfigOwnership recorded that consequence against
        // К3 from the other direction, and it is the same consequence as the five fields that came out of
        // the level file. Its key is retired from editor.json by name
        // (Editor::EditorPreferences::MigrateLoaded).
        int MSAASamples = 1;

        // Post-process AA. Was Core::SceneSettings::AA, so it travelled with the level: a machine that
        // could not afford SMAA had to edit a file that goes to everybody.
        AntiAliasingMode AA = AntiAliasingMode::FXAA;

        // Distance-based mesh level of detail. LOD0 (near) is byte-identical geometry, so off vs on only
        // changes what is drawn far from the camera — fidelity, not authoring.
        bool MeshLOD = true;

        TextureFilter TextureFilterMode = TextureFilter::Trilinear;
        int           Anisotropy        = 8; // 1/2/4/8/16x — used only in Anisotropic filter mode

        CloudQuality CloudQualityTier = CloudQuality::High;

        // Where this machine keeps the DerivedDataCache (Common/Content/DerivedDataCache.hpp; UE's
        // [DerivedDataBackendGraph] Path). Empty = <projectDir>/DerivedDataCache; relative = against the
        // project directory; absolute = anywhere. Per machine because it is a question of which disk,
        // and every entry under it is rebuildable, so pointing it at an empty directory costs time only.
        std::string DerivedDataCachePath;

        // --- EVERY OTHER KEY THE FILE HAPPENS TO CONTAIN -------------------------------------------
        // NOT A SETTING AND NOT A KEY OF ITS OWN. rfl::ExtraFields is spread flat at this struct's own
        // level on write and captures every top-level key the fields above did not claim on read, so the
        // file gains nothing called "UnknownKeys".
        //
        // WHY IT IS HERE FROM THE FIRST LINE OF THIS FILE'S LIFE. Every save is
        // `rfl::json::write( Get() )`, which rewrites the whole file from the struct THIS binary was
        // compiled with — so a key the binary has never heard of would be deleted by the act of saving
        // anything at all. К9 paid for that lesson in `editor.json` (two agents' builds erased the
        // owner's packaging fields within an hour), and this file has strictly MORE writers than that
        // one: the editor and the game, at any two vintages. A file with two hosts must not learn the
        // lesson a second time.
        //
        // It is not a compatibility shim and it does not keep legacy alive (contract §4). A key this
        // project DELETES on purpose is retired by name, not left unknown — nothing has been retired
        // from this file yet, because it is new.
        rfl::ExtraFields<rfl::Generic> UnknownKeys;

        // THE LIVE STATE. Everything that consumes one of these reads it from here; nothing keeps a copy
        // except the two derived pushes the Vulkan backend needs to read atomically
        // (Graphic::RenderConfig::TextureFilter / AnisotropyLevel, written by SceneRenderer::BeginScene
        // and by nothing else).
        static MachineSettings& Get();

        // Reads `file` into Get(), and REMEMBERS IT as the place Save() writes. Missing file = this
        // machine has never chosen anything, so the defaults above stand and nothing is written; a file
        // that exists and cannot be read or parsed is reported and the defaults stand.
        //
        // Called once per process, before any SceneRenderer is constructed — MSAASamples is baked at
        // Init and a late load would apply one start behind.
        static void Load( const std::filesystem::path& file );

        // The path Load() was given, or an empty path when this process never called it.
        static const std::filesystem::path& File();

        // THE USER JUST CHANGED SOMETHING. Writes the file, atomically, and returns whether the file now
        // holds these values.
        //
        // A save that would change nothing does not happen: the bytes are compared against what the file
        // ACTUALLY says, re-read immediately before the write, which is also where keys another build
        // owns are picked up (see UnknownKeys). Refuses, loudly, when Load() was never called — a store
        // with no path is not a store, and guessing one would be the silent fallback §1.4 forbids.
        static bool Save();
    };

    // Where a PACKAGED GAME keeps what belongs to this player on this machine — its save games, and this
    // file beside them. Created on demand.
    //
    //   macOS    ~/Library/Application Support/<product>
    //   Windows  %APPDATA%/<product>
    //   other    $XDG_DATA_HOME/<product>, or ~/.local/share/<product>
    //
    // NOT `~/.desertengine`: that directory is the ENGINE INSTALLATION's, shared by the editor, the
    // launcher and the tools, and a shipped game has no engine installation to belong to. Per PRODUCT
    // because two games on one machine are two different budgets, and `product` is the project's Name.
    //
    // A name is sanitised rather than trusted: it comes from a `.deproj` and lands in a path.
    std::filesystem::path GameUserDirectory( const std::string& product );
} // namespace Common::Settings

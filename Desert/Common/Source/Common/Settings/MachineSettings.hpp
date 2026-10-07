#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The unknown-key carrier below is a FIELD of the struct, so its type has to be visible here.
#include <Common/Json/Json.hpp>
#include <Common/Settings/DisplaySettings.hpp>
#include <Common/Settings/RecommendedQuality.hpp>
#include <Common/Settings/Scalability.hpp>

#include <optional>

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

    // The value domain of Scalability::Parameter::TextureFilter (Filtering.Texture). Live: the resolved value is
    // pushed into Graphic::RenderConfig by QualityState's listener and the samplers are recreated on a change.
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
        // THE QUALITY THIS MACHINE ASKED FOR (SCAL1): a level per group plus per-parameter overrides. Written only
        // through Scalability::QualityState (its Saver lands here); every renderer reads the RESOLVED values
        // (QualityState::Resolved()), never this. Replaces the six retired knobs AAMethod, MSAASamples,
        // TextureFilterMode, Anisotropy, MeshLOD and CloudQualityTier (see MigrateRetiredKeys).
        // ABSENT = this machine never chose: QualityBoot starts on StartingQuality's answer (the benchmark's
        // recommendation when it is valid for this device, UE's auto-detect on first run; else all High), and
        // the key appears in the file the first time a selection is applied.
        std::optional<Scalability::QualitySelection> Quality;
        // The benchmark's answer, cached with the device / driver / table identity it was measured on
        // (Scalability::CacheValid). Absent until the first benchmark run on this machine. Read by
        // StartingQuality (first run) and shown by the editor's Scalability panel.
        std::optional<Scalability::RecommendedQuality> Recommended;
        // How frames reach the screen (VSync), outside the quality groups (DisplaySettings.hpp). Read by
        // QualityBoot, which hands it to the window's swapchain; the only source of the present mode.
        Scalability::DisplaySettings Display;

        // Where this machine keeps the DerivedDataCache (Common/Content/DerivedDataCache.hpp; UE's
        // [DerivedDataBackendGraph] Path). Empty = <projectDir>/DerivedDataCache; relative = against the
        // project directory; absolute = anywhere. Per machine because it is a question of which disk,
        // and every entry under it is rebuildable, so pointing it at an empty directory costs time only.
        std::string DerivedDataCachePath;

        // --- EVERY OTHER KEY THE FILE HAPPENS TO CONTAIN -------------------------------------------
        // NOT A SETTING AND NOT A KEY OF ITS OWN. Json::CarriedKeys is spread flat at this struct's own
        // level on write and captures every top-level key the fields above did not claim on read, so the
        // file gains nothing called "UnknownKeys".
        //
        // WHY IT IS HERE FROM THE FIRST LINE OF THIS FILE'S LIFE. Every save is
        // `Json::WriteFileAtomic( file, Get() )`, which rewrites the whole file from the struct THIS binary was
        // compiled with — so a key the binary has never heard of would be deleted by the act of saving
        // anything at all. К9 paid for that lesson in `editor.json` (two agents' builds erased the
        // owner's packaging fields within an hour), and this file has strictly MORE writers than that
        // one: the editor and the game, at any two vintages. A file with two hosts must not learn the
        // lesson a second time.
        //
        // It is not a compatibility shim and it does not keep legacy alive (contract §4). A key this
        // project DELETES on purpose is retired by name, not left unknown: `AA` (post AA before the one
        // AAMethod), see MigrateRetiredKeys.
        Json::CarriedKeys UnknownKeys;

        // THE STORED STATE. Quality is read once, by the host, into Scalability::QualityState::Initialize; after
        // that QualityState owns the live selection and writes it back here through its Saver.
        static MachineSettings& Get();

        // Reads `file` into Get(), and REMEMBERS IT as the place Save() writes. Missing file = this
        // machine has never chosen anything, so the defaults above stand and nothing is written; a file
        // that exists and cannot be read or parsed is reported and the defaults stand.
        //
        // `table` is the parsed Scalability.json: the retired-key migration needs its High values (see
        // MigrateRetiredKeys), so the host parses the table before it loads this file.
        static void Load( const std::filesystem::path& file, const Scalability::ScalabilityTable& table );

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

        // Every level High, no override: the selection of a machine that never chose anything and has no valid
        // benchmark recommendation.
        [[nodiscard]] static Scalability::QualitySelection HighSelection();

        // What QualityBoot starts QualityState on (UE: a saved choice wins; with none, the hardware benchmark's
        // answer is applied once and saved). PURE.
        //   saved Quality                                  -> that selection, FromRecommended false;
        //   no Quality, Recommended valid for `device`     -> the recommended levels, FromRecommended true
        //                                                     (the host applies them through
        //                                                     QualityState::ApplyRecommended, which saves);
        //   no Quality, Recommended absent or stale        -> HighSelection(), FromRecommended false.
        struct StartingQuality
        {
            Scalability::QualitySelection Selection;
            bool                          FromRecommended                            = false;
            bool                          operator==( const StartingQuality& ) const = default;
        };
        [[nodiscard]] static StartingQuality StartFrom( const MachineSettings&                settings,
                                                        const Scalability::BenchmarkCacheKey& device );

        // What one migration did, for the log line and the tests.
        struct RetiredKeyMigration
        {
            int  KeysMoved = 0; // retired keys found in the file (each is removed from UnknownKeys)
            int  Overrides = 0; // of those, the ones whose value differs from the High table value
            bool operator==( const RetiredKeyMigration& ) const = default;
        };
        // THE MIGRATION OF THE RETIRED SCHEMA (SCAL1; expires with machine.json written before task/SCAL1), applied
        // by Load(). PURE: `rawJson` is the text `settings` was read from, `table` gives the High values.
        //
        // The retired keys are AAMethod, MSAASamples, TextureFilterMode, Anisotropy, MeshLOD, CloudQualityTier and the
        // older `AA` (the post filter before AAMethod). When the file holds any of them and NO `Quality` key, the
        // selection becomes all-High plus one override per retired value that differs from the High table value —
        // so an untouched machine comes out with zero overrides. MSAASamples moves only under AAMethod MSAA (it was
        // read only there); `AA` stands for AAMethod when AAMethod is absent (MSAASamples > 1 meant MSAA). When the
        // file already holds `Quality` (a newer build re-saved it and an older one added its keys back), Quality
        // wins and the retired keys are only dropped. Either way they leave UnknownKeys, so the next save writes
        // only `Quality`. A retired value that cannot be read is reported and not migrated.
        static RetiredKeyMigration MigrateRetiredKeys( MachineSettings& settings, std::string_view rawJson,
                                                       const Scalability::ScalabilityTable& table );
        // Removes the retired keys from `settings.UnknownKeys` without migrating them — the re-read before a save,
        // where the live Quality already holds this session's answer.
        static void DropRetiredKeys( MachineSettings& settings );
    };
    DESERT_JSON_STRUCT( MachineSettings, "MachineSettings", 1 )

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
    // A name is sanitised rather than trusted (SanitizeProductName, ProductName.hpp): it comes from a
    // `.deproj` and lands in a path.
    std::filesystem::path GameUserDirectory( const std::string& product );
} // namespace Common::Settings

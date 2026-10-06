#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// SCALABILITY — QUALITY GROUPS, THEIR LEVELS, AND THE ONE PLACE A CHOICE BECOMES WHAT A FRAME RUNS (SCAL1).
//
// THE UE SHAPE, KEPT. UE has sg.<Group>Quality CVars, levels 0..4 (Low..Epic) + Cinematic, a data file
// (BaseScalability.ini) mapping each (group, level) to concrete CVar values, per-CVar overrides on top of the
// group level, Scalability::SetQualityLevels() as the one apply point, and GameUserSettings persisting it.
// Every one of those has its counterpart below.
//
// WHAT IS BETTER THAN UE, ON PURPOSE.
//   * Parameters are a closed, TYPED list (Parameter + kParameterSpecs), not free CVar strings: an unknown key in
//     the data file is a load error, not a silently ignored line (UE's most common scalability bug).
//   * The device is known BEFORE any selection: resolution checks every value against the CapabilityCatalog and
//     reports each fallback (requested -> effective + reason) instead of a renderer clamping it later.
//   * Resolve() is PURE — (selection, table, catalog) -> resolved values + fallbacks — so every rule is testable
//     without a GPU (Desert/Tests/Engine/ScalabilityContract). Only Apply() has effects.
//
// LAYERS. This header is Common: the editor's panel and palette, the game's menu (Lua/UI) and the console all
// call the same QualityState API; the renderer READS ResolvedQuality and never sees a group or a level. No
// renderer reads a Level — if one does, the table has a missing parameter.
//
// MIGRATION OF THE EXISTING MachineSettings FIELDS (done by the implementation step, no legacy bridge, §4):
//     MachineSettings field   -> Parameter                 (Group)
//     AAMethod                -> AntiAliasingMethod        (AntiAliasing)
//     MSAASamples             -> AntiAliasingSamples       (AntiAliasing)
//     TextureFilterMode       -> TextureFilter             (Filtering)
//     Anisotropy              -> Anisotropy                (Filtering)
//     MeshLOD                 -> MeshLOD                   (ViewDistance)
//     CloudQualityTier        -> CloudQuality              (Effects; UE puts volumetric clouds under
//     sg.EffectsQuality)
//   The five fields are DELETED from MachineSettings and its JSON; MachineSettings gains one field,
//   `QualitySelection Quality`. MachineSettings::MigrateRetiredKeys gains one pass: when machine.json carries any
//   of the retired keys, each becomes an Override on a selection whose levels are all High — EXCEPT a retired
//   value equal to the High table value, which writes no override (so a machine that never touched a knob comes
//   out with zero overrides). The keys are removed; the next save writes only `Quality`. Logged once: file, how
//   many keys moved, how many became overrides. MachineSettings::EffectiveAA / ResolveAA / CommitAntiAliasing
//   and Graphic::RenderConfig::TextureFilter / AnisotropyLevel / MaxMSAASamples are replaced by ResolvedQuality
//   and QualityState::Apply (RenderConfig keeps only the atomic push the sampler thread reads, written by Apply's
//   listener, nothing else). The old AntiAliasingMethod enum in MachineSettings.hpp is deleted in favour of
//   Common::Scalability::AntiAliasingMethod (same leading values, so a stored int keeps its meaning).
namespace Common::Scalability
{
    // The groups — UE's sg.* set plus AntiAliasing and ResolutionScale, which UE keeps as sg.AntiAliasingQuality
    // and sg.ResolutionQuality. Order is the UI order and the JSON order.
    enum class Group : uint8_t
    {
        Textures = 0, // sg.TextureQuality — streaming pool, mip bias, cooked-size cap
        Filtering,    // texture filter + anisotropy (UE folds this into Textures; split because players tune it
                      // alone)
        Shadows,      // sg.ShadowQuality — shadow map resolution, cascades, filter taps, RT shadows
        GlobalIllumination, // sg.GlobalIlluminationQuality — GI resolve resolution / rays; never ON/OFF (authored,
                            // K1)
        Reflections,     // sg.ReflectionQuality — SSR steps / resolution, RT reflections
        PostProcess,     // sg.PostProcessQuality — bloom taps, DoF, motion blur quality
        Effects,         // sg.EffectsQuality — particles, volumetric clouds, fog quality
        ViewDistance,    // sg.ViewDistanceQuality — LOD distance scale, foliage/HLOD distances
        AntiAliasing,    // sg.AntiAliasingQuality — axis 1
        ResolutionScale, // sg.ResolutionQuality — axis 2
        Count
    };
    inline constexpr std::size_t kGroupCount = static_cast<std::size_t>( Group::Count );

    // Levels — UE's 0..4. Cinematic is a real level (offline / screenshots), not a synonym of Epic.
    enum class Level : uint8_t
    {
        Low = 0,
        Medium,
        High,
        Epic,
        Cinematic,
        Count
    };
    inline constexpr std::size_t kLevelCount = static_cast<std::size_t>( Level::Count );

    // EVERY QUALITY VALUE A RENDERER READS. A row exists only when a reader exists (contract §1.3): the comment
    // names it. A group whose rows are all placeholders (Textures today) is HIDDEN: IsGroupListed() is false, so no
    // UI or game API shows a slider for it, and a group with no row at all is refused by the loader when the data
    // file gives it levels - a group slider that moves nothing is a dead setting.
    //
    // PLACEHOLDERS (owner, 2026-10-06). A row whose spec says `Reader = std::nullopt` reserves a parameter for a
    // feature the engine does not have yet (TAA quality, ray-traced shadows/reflections/GI, upscaler sharpening,
    // texture streaming). The loader validates it like any row, Resolve passes it through (range + catalog), but
    // nothing applies it: ListedParameters() and IsGroupListed() leave it out of every UI and of the game API, and
    // QualityState::SetOverride refuses it. The day its feature lands the row gains its reader and appears. A
    // contract test pins both directions: every placeholder is hidden, every listed row names its reader.
    enum class Parameter : uint8_t
    {
        AntiAliasingMethod = 0, // SceneRenderer framebuffer setup + post AA pass. Values: AntiAliasingMethod
        AntiAliasingSamples,    // SceneRenderer framebuffer sample count; read only under MSAA. Values: 1/2/4/8
        RenderScalePercent,     // view-target size (RDG) and the upscaler pass. Values: RenderScaleRange
        Upscaler,               // upscaler pass. Values: Upscaler
        TextureFilter,          // sampler cache (RenderConfig push). Values: MachineSettings TextureFilter 0..3
        Anisotropy,             // sampler cache. Values: CapabilityCatalog::AnisotropyLevels
        MeshLOD,                // mesh LOD selection. Values: 0/1
        CloudQuality,           // Graphic::CloudQualityScale. Values: CloudQuality 0..2
        ShadowCascades,         // scene view's ShadowQuality::CascadeCount (MeshRenderer). 1..kMaxShadowCascades
        ShadowMapSize,          // ShadowQuality::ShadowMapSize, texels per cascade side. 512..4096
        ShadowDistance,         // ShadowQuality::MaxDistance, centimetres. 10 m .. 1 km
        // COST knobs of passes whose LOOK is authored per scene (PostProcessSettings): they scale what the pass
        // spends (steps, taps, mips), never its intensity - UE sg.* semantics.
        ReflectionMaxSteps,        // SSR trace march steps (SSRRenderer push constant). 8..64
        GlobalIlluminationSamples, // RSM GI gather taps per pixel (GIResolve.shader). 8..64
        AmbientOcclusionSamples,   // SSAO kernel taps (SSAORenderer). 4..32, SSAO.shader MAX_SAMPLES
        BloomMips,                 // bloom down/up-sample chain length (BloomRenderer). 2..kMaxBloomMips
        // ---- placeholders (Reader = nullopt) ----
        TextureMipBias,               // Textures: sampler LOD bias, in 1/100 mip
        TextureStreamingPoolMiB,      // Textures: resident texture budget
        ShadowRayTracing,             // Shadows: RayTracingMode
        GlobalIlluminationRayTracing, // GlobalIllumination: RayTracingMode
        ReflectionRayTracing,         // Reflections: RayTracingMode
        TemporalAAQuality,            // AntiAliasing: 0..3
        UpscalerSharpness,            // ResolutionScale: percent
        Count
    };
    inline constexpr std::size_t kParameterCount = static_cast<std::size_t>( Parameter::Count );

    // EVERY VALUE IS AN int. Enums are stored by their underlying value, bools as 0/1, percentages as percent.
    // Rejected: std::variant<bool,int,float> — no parameter needs a float, a variant makes every override in
    // machine.json carry a type tag, and a float level boundary (0.7 vs 0.70001) is a fallback nobody can read.
    // The first parameter that needs a fraction states it in its unit (percent, per-mille) instead.
    using ParameterValue = int32_t;

    // Which catalog list a parameter's values are checked against, if any.
    enum class CatalogList : uint8_t
    {
        None = 0, // a closed range in the spec is the whole truth (MeshLOD, TextureFilter, CloudQuality)
        AntiAliasingMethods,
        MSAACounts,
        Upscalers,
        RenderScale,
        AnisotropyLevels,
        RayTracingModes,
    };

    // ONE ROW PER PARAMETER, the single list every check is driven by: the JSON key, its group, its legal range
    // (before the device), and which catalog list narrows it. The census in ScalabilityContract asserts one row
    // per enum value, in enum order, with unique keys.
    struct ParameterSpec
    {
        Parameter        Id;
        Group            Owner;
        std::string_view Key; // JSON / machine.json / console name, e.g. "AntiAliasing.Method"
        ParameterValue   Min;
        ParameterValue   Max;
        CatalogList      NarrowedBy;
        // WHO READS THE RESOLVED VALUE (file / system), or std::nullopt for a placeholder: a reserved parameter
        // with no reader, hidden from every selector (see PLACEHOLDERS above).
        std::optional<std::string_view> Reader;
    };
    [[nodiscard]] constexpr bool IsPlaceholder( const ParameterSpec& spec )
    {
        return !spec.Reader.has_value();
    }
    [[nodiscard]] std::span<const ParameterSpec> ParameterSpecs();
    [[nodiscard]] const ParameterSpec&           SpecOf( Parameter parameter );
    [[nodiscard]] std::string_view               GroupKey( Group group ); // "Shadows", "AntiAliasing", ...
    [[nodiscard]] std::string_view               LevelKey( Level level ); // "Low" ... "Cinematic"
    // What a selector (editor panel, palette, game menu, console listing) may show: the parameters that have a
    // reader, and the groups owning at least one of them.
    [[nodiscard]] std::vector<Parameter> ListedParameters();
    [[nodiscard]] bool                   IsGroupListed( Group group );

    // A full set of values, one per parameter, indexed by Parameter. Unset slots do not exist: the table loader
    // refuses a level that does not set every parameter of its group.
    using ParameterValues = std::array<ParameterValue, kParameterCount>;

    // ---- The data file (UE BaseScalability.ini) ----------------------------------------------------------
    //
    // `Editor/Resources/Config/Scalability.json`, shipped by the packager in its Config tree, read with the
    // engine's own Common/Json (no third-party parser). Shape:
    //
    //   { "Version": 1,
    //     "Groups": {
    //       "AntiAliasing": {
    //         "Low":       { "AntiAliasing.Method": "FXAA", "AntiAliasing.Samples": 1, ... },
    //         ...
    //         "Cinematic": { "AntiAliasing.Method": "MSAA", "AntiAliasing.Samples": 8, ... } }, ... },
    //     "Recommend": {
    //       "Thresholds": { "Shadows": [ 40, 110, 250 ] },   // index >= t[i] -> level i+1; never Cinematic
    //       "MinVideoMemoryMiB": { "Textures": [ 0, 2048, 4096, 6144, 8192 ] }, // per level; VRAM gate
    //       "DeviceClass": { "Unknown": 10, "Integrated": 15, "AppleUnified": 60, "Discrete": 80 } } }
    //
    // Enum values are written by name. Every error names its JSON path and all of them are reported together. The
    // table is DATA, not code: changing what "Medium shadows" means is a data edit, never a recompile.
    struct ScalabilityTable
    {
        uint32_t Version = 0;
        // Values[group][level] — only the slots of that group's parameters are meaningful; the rest are the
        // spec Min and never read (Resolve takes each parameter from its own group's level).
        std::array<std::array<ParameterValues, kLevelCount>, kGroupCount> Values{};
        // Recommend thresholds per group: kLevelCount - 2 ascending perf-index boundaries (Low|Medium|High|Epic).
        std::array<std::array<float, kLevelCount - 2>, kGroupCount> RecommendThresholds{};

        // Recommend.MinVideoMemoryMiB: per group, per level, the device-local memory a level needs (0 = none).
        // RecommendLevels never recommends a level the machine's VideoMemory does not reach.
        std::array<std::array<uint32_t, kLevelCount>, kGroupCount> MinVideoMemoryMiB{};
        // Recommend.DeviceClass: the stand-in perf index per DeviceClass, used when the benchmark cannot time.
        std::array<float, 4> DeviceClassPerfIndex{};

        // PURE. Refuses, naming file position and the offending key/value, on: unknown group, level or parameter
        // key; a parameter listed under a group that does not own it; a level missing for a group that has
        // parameters; a level missing one of its group's parameters; a value outside the spec range; an enum
        // name that is not a value; a group WITHOUT parameters given levels (dead group); thresholds not strictly
        // ascending; a DeviceClass entry missing. Every error found, not the first.
        [[nodiscard]] static Common::ResultStr<ScalabilityTable> Parse( std::string_view jsonText );

        [[nodiscard]] ParameterValue ValueAt( Parameter parameter, Level level ) const;
    };

    // ---- The persisted choice (machine.json, MachineSettings::Quality) ----------------------------------

    // A player's/developer's value for ONE parameter that sits on top of its group's level (UE: setting a CVar
    // after sg.* applied it). Keyed by the spec Key, not the enum index, so reordering the enum never moves a
    // stored override.
    struct ParameterOverride
    {
        std::string    Key;
        ParameterValue Value = 0;

        bool operator==( const ParameterOverride& ) const = default;
    };

    struct QualitySelection
    {
        std::array<Level, kGroupCount> Levels{}; // per group; the UI's "Custom" is a group with an override
        std::vector<ParameterOverride> Overrides;

        bool operator==( const QualitySelection& ) const = default;
    };

    // ---- Resolution: the selection against the table and the device ------------------------------------

    // One value that does not run as asked, and why. Reason is a static string ("not offered by this device",
    // "the upscaler anti-aliases below 100 %", ...); FormatFallback renders the log line.
    struct Fallback
    {
        Parameter        Id;
        ParameterValue   Requested = 0;
        ParameterValue   Effective = 0;
        std::string_view Reason;

        bool operator==( const Fallback& ) const = default;
    };

    // How axis 2 renders this frame.
    enum class ScaleMode : uint8_t
    {
        Upscale,     // RenderScalePercent < 100: the Upscaler (never None) reconstructs output size
        Native,      // == 100
        Supersample, // > 100: SSAA, fixed downsample filter; Upscaler None
    };

    // WHAT EVERY RENDERER READS. The only product of the scalability system the renderer may see.
    struct ResolvedQuality
    {
        ParameterValues       Values{};  // effective value per parameter, after table, overrides and catalog
        std::vector<Fallback> Fallbacks; // every value that differs from the request, in Parameter order
        ScaleMode             Scale      = ScaleMode::Native;
        uint64_t              Generation = 0; // bumped by every Apply that changed Values; renderers compare it

        template <typename T>
        [[nodiscard]] T As( Parameter parameter ) const
        {
            return static_cast<T>( Values[static_cast<std::size_t>( parameter )] );
        }

        bool operator==( const ResolvedQuality& ) const = default;
    };

    // THE RULES OF RESOLUTION, in order, and the reason each one writes:
    //   1. Every parameter takes its group's level value from the table.
    //   2. An override replaces it (an override whose Key no spec has is dropped and reported — the file came
    //      from a newer build; it stays in machine.json through the UnknownKeys carrier, not here).
    //   3. A value outside its spec range is clamped ("outside the parameter's range").
    //   4. A value its catalog list does not offer walks DOWN the list to the nearest offered value below it;
    //      if none, the list's front ("not offered by this device"). MSAACounts/AnisotropyLevels: largest offered
    //      <= requested. Upscalers: DLSS/XeSS/MetalFX/FSR -> TAAU. AA methods: DLAA/FSRNative -> TAA.
    //   5. Axis coupling: RenderScale < 100 and Upscaler None -> TAAU ("an upscaler is required below 100 %");
    //      RenderScale >= 100 and Upscaler != None -> None ("upscaler unused at native/supersampled scale");
    //      RenderScale < 100 -> AntiAliasingMethod effective = TAA unless it is DLAA/FSRNative matching the
    //      upscaler, which become that upscaler's temporal pass ("the upscaler anti-aliases below 100 %");
    //      AntiAliasingMethod MSAA with RenderScale != 100 -> FXAA ("MSAA renders at native scale only").
    //   6. The render-path rule of MachineSettings' EffectiveAntiAliasing table (MSAA on a path that cannot
    //      multisample) is NOT here: it is per path, and stays a pure function of (ResolvedQuality, path) —
    //      ResolveAntiAliasingForPath below.
    // PURE: no log, no globals. Apply() logs.
    [[nodiscard]] ResolvedQuality Resolve( const QualitySelection& selection, const ScalabilityTable& table,
                                           const CapabilityCatalog& catalog );

    // Per-path AA: what the scene framebuffer and the post pass run on a path that can / cannot multisample.
    struct PathAntiAliasing
    {
        AntiAliasingMethod Method      = AntiAliasingMethod::FXAA;
        int                Samples     = 1;
        AntiAliasingMethod PostProcess = AntiAliasingMethod::FXAA; // None under MSAA and under temporal methods
        std::string_view   Reason; // empty when the path runs the resolved method unchanged

        bool operator==( const PathAntiAliasing& ) const = default;
    };
    [[nodiscard]] PathAntiAliasing ResolveAntiAliasingForPath( const ResolvedQuality& resolved,
                                                               bool                   pathSupportsMSAA );

    // "[Scalability] AntiAliasing.Method: DLAA -> TAA (not offered by this device)"
    [[nodiscard]] std::string FormatFallback( const Fallback& fallback );

    // ---- Menu presets: one entry of a game's AA menu, decomposed into the two axes -----------------------
    struct AntiAliasingPreset
    {
        std::string_view   Key; // "DLSS.Quality", "FSR.Balanced", "Native.TAA", "MSAA.4x", "SSAA.150", ...
        AntiAliasingMethod Method             = AntiAliasingMethod::TAA;
        int                Samples            = 1;
        int                RenderScalePercent = 100;
        Upscaler           Upscale            = Upscaler::None;
    };
    // The engine's preset list (UE's DLSS/FSR mode names: Quality 67 %, Balanced 58 %, Performance 50 %).
    [[nodiscard]] std::span<const AntiAliasingPreset> AntiAliasingPresets();
    // The presets this device can run unchanged — what a menu lists. A preset that would resolve with a
    // fallback is not offered (a menu entry that silently becomes another one is a dead setting).
    [[nodiscard]] std::vector<AntiAliasingPreset> OfferedPresets( const CapabilityCatalog& catalog );
    // The preset as four overrides (AntiAliasing.Method, .Samples, Resolution.Percent, Resolution.Upscaler).
    [[nodiscard]] std::vector<ParameterOverride> Decompose( const AntiAliasingPreset& preset );

    // ---- THE ONE APPLY POINT, and the game-facing API ----------------------------------------------------
    //
    // UE's Scalability::SetQualityLevels. Process-wide like MachineSettings::Get(): one machine, one quality.
    // The editor panel, the palette, the game's menu (Lua binding) and the console all call these and nothing
    // else; nobody writes MachineSettings::Quality directly.
    class QualityState
    {
    public:
        // Once, after the device exists (the catalog) and machine.json is loaded (the selection). The table is
        // parsed by the host from Scalability.json; a parse failure stops the host — there is no built-in table to
        // fall back to (a second copy of the levels in code would be the two-sources defect).
        //
        // `save` persists a selection (the host's machine.json writer); Apply calls it after publishing. Injected
        // so the one apply point does not depend on how the host stores machine settings, and so tests mock it.
        using Saver = Common::BoolResultStr ( * )( const QualitySelection& selection );
        static void Initialize( ScalabilityTable table, CapabilityCatalog catalog, QualitySelection saved,
                                Saver save );
        // A device loss / new device: same selection, new catalog -> re-resolve, report what changed.
        static void ReplaceCatalog( CapabilityCatalog catalog );

        // What Apply reports: the new fallbacks (each logged once, here — the only place a quality fallback is
        // logged) and whether machine.json now holds the selection.
        struct ApplyReport
        {
            std::vector<Fallback> NewFallbacks; // fallbacks not present in the previous resolution
            bool                  ValuesChanged = false;
            bool                  Saved         = false;
            std::string_view      Refused; // non-empty: nothing was applied, and why (logged at ERROR)
        };
        // THE apply point: resolve, log each NEW fallback once, publish ResolvedQuality (Generation + 1 when
        // values changed), notify listeners, save machine.json. Every setter below is this with an edited
        // selection.
        static ApplyReport Apply( const QualitySelection& selection );

        static ApplyReport SetGroupLevel( Group group,
                                          Level level ); // drops that group's overrides (UE behaviour)
        static ApplyReport SetAllGroups( Level level );  // a menu's "Overall quality"; drops all overrides
        static ApplyReport SetOverride( Parameter parameter, ParameterValue value );
        static ApplyReport ClearOverride( Parameter parameter );
        static ApplyReport ApplyPreset( const AntiAliasingPreset& preset ); // Decompose + overrides
        static ApplyReport ApplyRecommended( const std::array<Level, kGroupCount>& levels ); // benchmark result

        [[nodiscard]] static const QualitySelection&  Selection();
        [[nodiscard]] static const ResolvedQuality&   Resolved();
        [[nodiscard]] static const CapabilityCatalog& Catalog();
        [[nodiscard]] static const ScalabilityTable&  Table();

        // Renderers and the sampler push subscribe; called on the main thread inside Apply, after publishing.
        using Listener = void ( * )( const ResolvedQuality& resolved, void* user );
        static void Subscribe( Listener listener, void* user );
        static void Unsubscribe( Listener listener, void* user );
    };
} // namespace Common::Scalability

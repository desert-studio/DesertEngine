#pragma once

#include <glm/glm.hpp>

#include <Common/Core/Units.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

namespace Desert::Core
{
    // THREE ENUMS LEFT THIS HEADER WITH THE FIELDS THAT USED THEM (К3): AntiAliasingMode, TextureFilter
    // and CloudQuality now live in Common/Settings/MachineSettings.hpp, because what they name is what a
    // MACHINE can afford and the packaged game has to be able to say it too. The texture-filter enum
    // additionally existed TWICE — here and in RenderConfig — under a "Must match" comment with nothing
    // asserting the match; both spellings are gone and Common::Settings::TextureFilter is the only one.

    // Rendering path. Forward = the classic one-pass lit shading. Deferred = a G-buffer pass + a
    // screen-space lighting pass, which scales to many dynamic lights (city lamps/windows) and unlocks
    // screen-space GI/AO.
    //
    // DEFERRED IS THE DEFAULT AND WHAT ESSENTIALLY EVERYTHING RUNS. Recounted 2026-09-14 over
    // Editor/Resources/Assets/Scenes (Autosave/ excluded — it holds copies of scenes already counted,
    // and an earlier revision of this comment said "51 of 53" because it swept them in): 88 scenes, of
    // which 81 write RenderingPath 1 and 7 write 0. Every scene now states the key, so nothing inherits
    // the default any more. The Forward seven are all MAT_Probe* fixtures: BatchStress, CascadeSeam,
    // Clouds, GeoShadows, GraphBatchStress, Shadows and UnlitShadows.
    //
    // The 2026-09-04 revision said "46 of 51" and was already three generations of scenes out of date
    // when Г26 read it — and Г26 quoted it into a commit message before recounting. A count in a comment
    // has no way of ageing out loud; recount before citing one. The comment also used to describe Forward
    // as "the default" and "the safe path", which had not been true for a long time and made the two
    // paths look interchangeable.
    //
    // The two paths now share one ambient model AND one direct-lighting model: both compile
    // Mesh/AmbientIBL.glslh and Mesh/DirectLighting.glslh. Until 2026-09-03 the composite floored its
    // ambient at a flat vec3(0.08) and read NEITHER environment cube, which is why the owner saw ground
    // and buildings almost black under a bright sky and why flipping this one field lit them; until
    // 2026-09-04 the forward shaders also omitted the Lambertian 1/pi that the composite applied, making
    // the same sun pi times brighter on that path. Docs/Sky/HANDOVER.md item 2 records the first
    // measurement.
    //
    // WHAT STILL MAKES THE TWO DISAGREE ON THE GROUND is cloud shadow, and it is asymmetric by
    // omission: CloudShadowFactor is applied in Programs/Deferred/DeferredLighting.shader and in no
    // other shader, so the forward path and Terrain.shader stand in full sun under a cloud that darkens
    // the deferred ground by a factor of 1.68 (64.93 against 109.11, measured by Р20).
    //
    // What still differs is what the G-buffer can carry: the deferred path shades from albedo, a single
    // world normal, metallic and roughness, so anything a forward shader does with per-material state
    // that the G-buffer has no channel for is unavailable to it.
    enum class RenderPath : int
    {
        Forward  = 0,
        Deferred = 1,
    };

    // WHETHER A PATH CAN MULTISAMPLE, the input MachineSettings::EffectiveAA takes (AA2). Forward shades
    // every sample it rasterizes; Deferred shades one G-buffer sample per pixel, so MSAA there smooths
    // only forward-drawn objects and is replaced by FXAA (see Common::Settings::EffectiveAntiAliasing).
    constexpr bool RenderPathSupportsMSAA( const RenderPath path )
    {
        return path == RenderPath::Forward;
    }

    // Reflected (REFLECT/PROPERTY) so the whole block (de)serializes generically via the reflection
    // serializer (no hand-written mirror) and the editor can build its panel from the same metadata.
    struct SceneSettings
    {
        REFLECT()

        // Selection outline (Jump Flood) moved OUT of scene settings into EditorPreferences: it is an
        // editor-only viewport visualization (runtime builds have no selection), not a scene property.
        // See Editor::EditorPreferences (OutlineColor/Width/Smoothness/EnableOutline), pushed to the
        // renderer each frame via SceneRenderer::SetOutlineSettings.
        //
        // THE TEN DEBUG-VISUALIZATION FIELDS FOLLOWED IT OUT (К2), for the same reason and with harder
        // evidence: ShowGrid, ShowColliders, ShowBoundingBoxes + its colour and width, WireframeMode,
        // ShowNormals, LightingDebug, ShadowDebug and DeferredDebug are what a VIEW is drawing on top of
        // the world, not anything the world is. They now live in Graphic::DebugViewState, one per
        // SceneRenderer, defaulted to "show nothing" and pushed in from Editor::EditorPreferences the way
        // the outline is. Scene schema v12 -> v13 strips them from every file (Tools/SceneMigrator), and
        // Desert/Tests/Engine/SceneDebugFields keeps them out — of this struct AND of every .desce on disk.
        //
        // WHAT THIS STRUCT IS, THEN. A LEVEL's world policy — UE's World Settings: the render path, gravity,
        // the splash. Things a level designer authors and expects to travel with the level.
        //
        // AND WHAT LEFT IT FOR ITS UE HOMES (SET1, scene schema v36). The grade — tonemapper, exposure,
        // bloom, lens flare, GI, SSR, SSAO — is ECS::PostProcessVolumeData::Settings on PostProcessVolume
        // entities, blended per view by Graphic::ResolveViewSettings (UE's Post Process Volume and
        // FFinalPostProcessSettings). The shadow policy — Shadows, Shadow Bias, Cascade Split Lambda — is
        // on the DirectionalLight that casts them (UE: the light's Cascaded Shadow Maps section). The
        // SceneMigrator step moved every scene's values into an Unbound volume and onto its light.
        //
        // In one sentence (К1): a .desce holds WHAT THE WORLD IS — its entities and the level-wide policy a
        // designer authors and expects to travel with the level. It does NOT hold what a VIEWER is doing on
        // top of the world (К2 took ten such fields out, to Graphic::DebugViewState), and it does NOT hold
        // what a MACHINE can afford. The three-question procedure that decides where a NEW field goes, and
        // the census that goes red when one lands in the wrong file, are in
        // Desert/Tests/Engine/ConfigOwnership — which enumerates this struct through the reflection registry,
        // i.e. through the same table SceneSerializer writes the block with.
        //
        // THE FIVE MACHINE-QUALITY FIELDS ARE GONE (К3), and this is where the note naming them used to
        // stand. AA, MeshLOD, TextureFilterMode, Anisotropy and CloudQualityTier described what a MACHINE
        // can afford, so a weak machine could not turn the picture down without editing a file that goes
        // to everybody: they are in Common::Settings::MachineSettings now, one schema read by BOTH hosts
        // from two places (`~/.desertengine/machine.json` for the editor, the game's own user directory
        // for a packaged build). editor.json was NOT a valid destination — all five are read by
        // SceneRenderer, which the packaged game runs, and the Runtime never opens that file — which is
        // why the answer had to be a store rather than a move. Scene schema v14 -> v15 strips them from
        // every file (Tools/SceneMigrator, Migration::kRetiredKeys).
        //

        // Rendering path. Default is Deferred — see the enum's own comment for what that costs.
        PROPERTY( DisplayName( "Render Path" ), Category( "Rendering" ) )
        RenderPath RenderingPath = RenderPath::Deferred;

        PROPERTY( DisplayName( "Gravity" ), Category( "Physics" ), Range( 0.0f, 5000.0f ) )
        float Gravity = 981.0f; // For physics simulation (cm/s^2 — 1 unit = 1 cm)
        // "Pause Simulation" used to sit here. It was reflected, serialized and shown beside Gravity, and
        // read by NOTHING — the editor's transport owns pausing, as runtime state rather than scene data.
        // Sixteen probe scenes had authored it `true` and got nothing, which is the exact defect §1.3
        // exists to forbid; deleted by Д26 rather than wired, because a scene file that ships "physics is
        // paused, forever" is a trap and the transport already answers the real need.

        // WIND STOOD HERE - three fields, stated by all 86 scenes, read by NOBODY (Г26).
        //
        // Their one consumer was the procedural grass generator, which Г25 removed because grass arrives
        // as a mesh asset now; Г25 kept the fields and wired them to SceneRenderer::GetWind() so the next
        // reader would find them ready. There was no next reader, and a field waiting for a future
        // consumer is indistinguishable from a forgotten one - the dead setting §1.3 of the contract
        // forbids, in its purest form: three sliders in every scene's Details moving nothing at all.
        //
        // WHEN LEAVES SWAY, WIND COMES BACK WITH THE THING THAT SWAYS, and with a unit and a meaning
        // chosen by whatever actually reads it - rather than a heading in degrees, an amplitude in
        // nothing, and a "gustiness" whose scale was never defined against anything. Cutting it also took
        // the last std::chrono::steady_clock read out of the frame's per-frame state (SceneRenderer), the
        // input that made a repeat shot of a grass scene differ from itself by 14 % of its pixels.
        //
        // The retirement is written where it will be found: kRetiredKeys in Tools/SceneMigrator, which
        // drops the three keys from the files at scene schema v19.

        // Water moved OUT of global scene settings: it is a gameplay value, not a render setting. It now
        // lives on the spawned "Water" entity (World.spawnWater drops a plane at the level); World.waterLevel
        // reads that entity's height, so the swim script keeps working without a global knob here.

        // Time of Day was removed: the sun's single source of truth is the directional-light ENTITY
        // (its position encodes the direction; sky + lighting follow it). Old scene files may still
        // carry the fields — unknown keys are ignored on load.

        // Splash screen: an image the standalone Runtime shows full-screen when THIS scene loads (the boot
        // scene's splash is the game's startup splash). Duration 0 = no splash; Fade = in/out seconds.
        // Asset<TextureAsset> for the reason UIPanelData::Sprite gives: it names the asset type to the
        // resolver, without which the slot serializes as an empty string. SceneSerializer had to gain a
        // resolver for this one field — the settings block was written without one, which is what put a
        // raw 64-bit handle through the JSON double round trip.
        // UE's GameMode DefaultPawnClass: the prefab Play spawns at the level's PlayerStart and gives the
        // player (Core::BeginPlay). Unset = a level without a player — a cinematic, a benchmark.
        PROPERTY( DisplayName( "Default Pawn" ), Category( "Game Mode" ), Asset<PrefabAsset> )
        Assets::AssetHandle DefaultPawn;

        PROPERTY( DisplayName( "Splash Sprite" ), Category( "Splash" ), Asset<TextureAsset> )
        Assets::AssetHandle SplashSprite;
        PROPERTY( DisplayName( "Splash Duration" ), Category( "Splash" ), Range( 0.0f, 10.0f ) )
        float SplashDuration = 0.0f;
        PROPERTY( DisplayName( "Splash Fade" ), Category( "Splash" ), Range( 0.0f, 3.0f ) )
        float SplashFade = 0.4f;
    };
} // namespace Desert::Core
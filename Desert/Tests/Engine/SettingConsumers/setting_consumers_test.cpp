// Does every exposed setting actually reach something?
//
// A slider that moves nothing is the failure mode this programme was written to avoid, and it is not
// caught by any build: an unread field compiles, serializes, appears in Details and does nothing at all.
// So the reflected field list of EVERY reflected type is enumerated here and matched against an explicit
// table that says, for every single field, WHO consumes it.
//
// A row has exactly one of three kinds:
//
//   * WIRED   - names a source file that must contain an ANCHORED READ of the field (see
//               setting_consumers_reader.hpp). Delete the read and this suite goes red.
//   * PENDING - names the TASK that owes the field a consumer. The per-component counts at the bottom
//               pin how many exist, so a field joining a component without a reader is a reviewable
//               edit rather than a silent one.
//   * DEAD    - the field has NO consumer today, named one by one with the reason. This is the known
//               debt list: `kKnownDeadSettings` restates every one of them and a test pins the list
//               exactly, so neither a new dead setting nor the repair of an old one passes quietly.
//
// Every reflected field must appear in exactly one row, and every reflected TYPE must appear in the
// census, so a field or a type added tomorrow fails this suite until somebody decides which kind it is.
// That decision is the point.
//
// WHAT CHANGED IN Д23, AND WHY IT WAS THE WHOLE TASK. Two things were measured and both were bad.
//
//   1. COVERAGE. This census guarded FOUR of the engine's THIRTY-SEVEN reflected types (sky, fog,
//      clouds, hero clouds), later five. Twenty-one of the rest are the UI components - the ones with
//      the most fields and the least GPU, i.e. the ones where a dead setting is invisible. All 39 types
//      are covered now (Ю12 added the two overlay ones), and `EveryReflectedTypeIsUnderThisCensus` makes
//      the 40th fail here first.
//   2. THE ROW SHAPE. A WIRED row used to assert that the named file mentions the field's NAME. `Sprite`
//      belongs to the canvas, the button, the panel AND the image, so when У3 deleted the canvas's own
//      background draw from UICanvasRenderer2D.cpp the row stayed green - proven by mutation, not by
//      argument. A row now asserts an anchored read: a member access of the field on a receiver that the
//      same file binds to THIS type. The mutation reddens it; renaming a local variable does not.

#include "setting_consumers_reader.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using Desert::Reflection::FieldInfo;
using Desert::Reflection::ReflectionRegistry;
using Desert::Reflection::TypeInfo;

namespace
{
    struct Row
    {
        const char* Field;

        // Exactly one of these is set. Where = a repo-relative source path that must contain an anchored
        // read; Task = the task that owes a consumer; Dead = why nothing reads it, and it must also be
        // named in kKnownDeadSettings below.
        const char* Where = nullptr;
        const char* Task  = nullptr;
        const char* Dead  = nullptr;

        // THE OTHER END OF THE CHAIN, and the reason Г26 added it. `Where` proves that SOMEBODY reads
        // the field; it cannot prove that the value reaches a frame. The procedural terrain's RockMode had a
        // WIRED row that was true — its ECS system packed it into the draw command's LayerModes.y — and the
        // terrain shader never read LayerModes.y and never sampled the green splat channel, so the
        // `Rock (G)` brush the editor offers painted nothing anybody could see. The census was satisfied
        // one link before the frame. The same shape retired the three Wind fields above: BeginScene read
        // all three, into a struct nothing read.
        //
        // A Frame anchor closes the gap for the rows that can state it: `Frame` is the file the value
        // must SURVIVE to (a shader, in every case so far) and `FrameRead` is the exact expression that
        // must appear in it, outside comments and string literals. It is not derivable — the packing is
        // `glm::vec3(GrassMode, RockMode, SnowMode)` widened to a vec4 and named `u_T.LayerModes`, and
        // nothing in either file states the correspondence — so it is written down once, per row, and
        // then checked. Where it is MANDATORY rather than optional is decided from reflection: see
        // EveryLayerModeFieldStatesWhereItReachesTheFrame.
        const char* Frame     = nullptr;
        const char* FrameRead = nullptr;
    };

    // How a consumer file is allowed to get hold of a value of this type. `Component` is the ECS wrapper
    // whose `Data` member holds it (NOT derivable from the type name - `DirectionalLightData` lives in
    // `DirectionLightComponent` and `UITextData` in `UITextComponent2D`, and a convention that guessed
    // would have silently found no receivers and reported ten live fields as dead). `Accessor` is a
    // getter that returns the type, which is how a non-component like SceneSettings is reached.
    struct Census
    {
        const char* Type;
        const char* Component;
        const char* Accessor;
        const Row*  Rows;
        std::size_t Count;
    };

    // ------------------------------------------------------------------------------------------------
    // Sky: every field is wired. The artistic-gradient group through the sky pass and the IBL bake, the
    // physical-atmosphere group through the LUT passes and the Phase 2 sky pass, and the
    // aerial-perspective group through the Phase 3 froxel volume and the atmospheric-fog pass.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kSkySettings = "Desert/Desert/Source/Engine/Graphic/SkySettings.hpp";
    constexpr const char* kTimeOfDay   = "Desert/Desert/Source/Engine/ECS/System/TimeOfDayECSSystem.hpp";
    constexpr const char* kCollector   = "Desert/Desert/Source/Engine/ECS/System/SkyboxECSSystem.hpp";
    constexpr const char* kSkyWidget =
         "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/SkyAtmosphereComponent.cpp";

    constexpr Row kSkyRows[] = {
         // The collector decides whether the atmosphere drives the frame at all.
         { "Enabled", kCollector },

         // MakeSkySettings is the one funnel from component to render command; from there the palette
         // reaches the screen pass and the IBL bake through the same packed block.
         { "SkyBrightness", kSkySettings },
         { "HorizonFalloff", kSkySettings },
         { "ZenithColor", kSkySettings },
         { "HorizonColor", kSkySettings },
         { "GroundColor", kSkySettings },
         { "NightColor", kSkySettings },
         { "SunIntensity", kSkySettings },
         { "SunColor", kSkySettings },
         { "SunAngularDiameter", kSkySettings },
         { "SunGlow", kSkySettings },
         { "SunsetColor", kSkySettings },
         { "SunsetIntensity", kSkySettings },
         { "StarIntensity", kSkySettings },

         // The time-of-day driver turns these five into the sun's transform.
         { "DriveSunFromTimeOfDay", kTimeOfDay },
         { "TimeOfDay", kTimeOfDay },
         { "DayLengthSeconds", kTimeOfDay },
         { "Latitude", kTimeOfDay },
         { "NorthOffset", kTimeOfDay },

         // Environment-bake policy and size, carried in the same settings block.
         { "AutoRebakeEnvironment", kSkySettings },
         { "RebakeSunAngleThreshold", kSkySettings },
         { "EnvironmentResolution", kSkySettings },

         // Display-only state, and the widget is what maintains it.
         { "ActivePreset", kSkyWidget },

         // Converted to world units on the C++ side.
         { "PlanetRadius", kSkySettings },

         // ---- The physical atmosphere (Phase 0/1 of the Sky Atmosphere programme) ------------------
         // The medium group funnels through MakeSkySettings into the sky payload's medium block, where
         // the SkyTransmittanceLut / SkyMultiScatterLut compute passes read it — a fingerprint change
         // re-dispatches both, so each of these fields moves real GPU texels today.
         { "Model", kSkySettings }, // gates the LUT dispatch (SkyboxRenderer::ExecuteAtmosphereLuts)
         { "AtmosphereHeight", kSkySettings },
         { "MultiScatteringFactor", kSkySettings },
         { "GroundAlbedo", kSkySettings },
         { "RayleighScatteringScale", kSkySettings },
         { "RayleighScattering", kSkySettings },
         { "RayleighExponentialDistribution", kSkySettings },
         { "MieScatteringScale", kSkySettings },
         { "MieScattering", kSkySettings },
         { "MieAbsorptionScale", kSkySettings },
         { "MieAbsorption", kSkySettings },
         { "MieExponentialDistribution", kSkySettings },
         { "OtherAbsorptionScale", kSkySettings },
         { "OtherAbsorption", kSkySettings },
         { "AbsorptionTipAltitude", kSkySettings },
         { "AbsorptionTipValue", kSkySettings },
         { "AbsorptionTentWidth", kSkySettings },

         // Wired by Phase 2: MieAnisotropy is the Cornette-Shanks g of the scattering integrator
         // (Common/SkyScattering.glslh via the SkyViewLut / BakeProceduralSky marches); the two
         // art-direction tints funnel through MakeSkySettings into the payload's Phase 2 lanes, read
         // by the physical sky pass (SkyLuminanceFactor, on-screen pixels only) and inside every
         // scattering integration (SkyAndAerialPerspectiveLuminanceFactor).
         { "MieAnisotropy", kSkySettings },
         { "SkyLuminanceFactor", kSkySettings },
         { "SkyAndAerialPerspectiveLuminanceFactor", kSkySettings },

         // Wired by Phase 3: all three funnel through MakeSkySettings into SkySettings, from where
         // SkyboxRenderer fills the 32x32x16 aerial-perspective volume (start depth and distance, on
         // the fill's push block) and the atmospheric-fog pass reads it (distance and view-distance
         // scale, published on AtmosphereEnv). Every one of them moves real froxels today.
         { "AerialPerspectiveViewDistanceScale", kSkySettings },
         { "AerialPerspectiveStartDepth", kSkySettings },
         { "AerialPerspectiveDistance", kSkySettings },
    };

    // ------------------------------------------------------------------------------------------------
    // Height fog: the component and its pass shipped together (Sky plan Phase 5), so every field is
    // WIRED - nothing pending. One funnel consumes them: PackFogParams in FogPayload.hpp turns each
    // field into the GPU block the fog pass evaluates; Enabled is the renderer's own dispatch gate.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kFogPayload = "Desert/Desert/Source/Engine/Graphic/Fog/FogPayload.hpp";
    constexpr const char* kFogRenderer =
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Fog/HeightFogRenderer.cpp";

    constexpr Row kFogRows[] = {
         { "Enabled", kFogRenderer }, // the zero-cost gate: off means no allocation and no dispatch

         { "FogDensity", kFogPayload },
         { "FogHeightFalloff", kFogPayload },
         { "FogInscatteringLuminance", kFogPayload },
         { "SkyAtmosphereAmbientContributionColorScale", kFogPayload },
         { "FogMaxOpacity", kFogPayload },
         { "StartDistance", kFogPayload },
         { "FogCutoffDistance", kFogPayload },

         { "SecondFogDensity", kFogPayload },
         { "SecondFogHeightFalloff", kFogPayload },
         { "SecondFogHeightOffset", kFogPayload },

         { "DirectionalInscatteringExponent", kFogPayload },
         { "DirectionalInscatteringStartDistance", kFogPayload },
         { "DirectionalInscatteringLuminance", kFogPayload },
    };

    // ------------------------------------------------------------------------------------------------
    // Volumetric clouds: every field is WIRED, and there are three consumers rather than one because the
    // component's fields reach the GPU by three different routes.
    //
    //   * PackCloudParams turns the per-frame settings into the twelve-vec4 block the march reads. It
    //     lives in CloudPayload.hpp.
    //   * The ECS system owns the timestep, so the two wind fields are integrated there into the offset
    //     the packer is handed - the component carries no accumulated state of its own.
    //   * Enabled is the renderer's dispatch gate: off allocates nothing and dispatches nothing, which is
    //     a decision the packer cannot make because it runs after it.
    //
    // WHAT USED TO BE HERE AND IS NOT. Four rows - WeatherSeed, WeatherOctaves, DetailSeed and
    // DetailOctaves - pointed at a bake key that turned them into the push constant of a compute pass.
    // That pass is gone: the noise volume is an asset generated offline, its seed and lattice periods live
    // in the container's own header, and the component names the volume instead of describing how to bake
    // one. Four rows removed rather than repointed, because there is nothing left for them to point at.
    //
    // And four more since: LayerBottomAltitude and LayerThickness stated the shell by hand, which the
    // cloud type now states in kilometres and the packer computes; the old scalar CloudType and its
    // variance drove one analytic profile curve, which is now a per-type AUTHORED curve living in the
    // type's own asset. Every one of the four was removed rather than repointed.
    //
    // And ONE more with T1: NoiseVolume. It was not removed - it MOVED, onto the cloud type asset, because
    // the character of a cloud's edge is a property of the kind of cloud rather than of the layer's
    // weather.
    //
    // AND THEN THIRTY-THREE AT ONCE, WITH O1: everything that described the LOOK - the four type slots,
    // the weather, the placement, the painted layout, the per-sample detail and the medium's lighting -
    // became parameters of the Volume-domain material (CloudRaymarch.shader's Properties block), authored
    // in a `.demat` the one new row below names. Their fifth-link guarantee did not lapse with the move:
    // Desert/Tests/Engine/CloudMaterialSchema holds the schema-side census on the same terms this table
    // holds the reflected one.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kCloudPayload = "Desert/Desert/Source/Engine/Graphic/Clouds/CloudPayload.hpp";
    constexpr const char* kCloudSystem  = "Desert/Desert/Source/Engine/ECS/System/VolumetricCloudECSSystem.hpp";
    constexpr const char* kCloudRenderer =
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp";

    constexpr Row kCloudRows[] = {
         { "Enabled", kCloudRenderer }, // the zero-cost gate: off means no allocation and no dispatch

         // THE MATERIAL - the one field O1 added while thirty-three left. The renderer resolves it
         // through Runtime::MaterialService into Graphic::CloudMaterialValues (schema defaults, then the
         // `.demat` chain), and everything the moved fields used to feed now reads THAT. The moved
         // fields' own census lives with the schema: Desert/Tests/Engine/CloudMaterialSchema pins every
         // schema parameter to a CloudMaterialValues field in both directions, which is this suite's
         // §1.3 guarantee restated for parameters that are no longer reflected C++.
         { "Material", kCloudRenderer },

         // Cloud Layer - the shell the march intersects and the budgets of the trace. The cloud TYPES
         // that build the shell are MATERIAL parameters now (CloudType1..4 of the CloudRaymarch schema);
         // the component keeps the planet and the ray budgets, name for name UE's.
         { "PlanetRadius", kCloudPayload },

         // The SCENE's own lift of the deck, and the one row here whose consumer is not a packer at all.
         // Graphic::CloudLiftSpeciesSet reads it and moves the resolved CLOUD TYPE SHAPES by it, which is
         // upstream of every shell in the subsystem — the packed CloudGpuPayload::Layer, the bake's
         // LayerBottomKm and the bodies the bake places all follow from the lifted array. Wired to the
         // packer's file because that is where the read is; the renderer's ResolveSpecies is the caller.
         { "LayerAltitudeOffset", kCloudPayload },

         { "MaxViewDistance", kCloudPayload },
         { "TracingStartDistance", kCloudPayload },
         { "TracingStartMaxDistance", kCloudPayload },

         // Weather's one surviving field: the REGION is a memory budget (how much world the modelling
         // volume covers), not a look - the look's own weather (Coverage, the tile, the seed) moved into
         // the material with everything else.
         { "RegionSize", kCloudRenderer },

         // Detail's two survivors: march control at the camera, not the cloud's look.
         { "NearFadeStartDistance", kCloudPayload },
         { "NearFadeEndDistance", kCloudPayload },

         // Lighting - the pass-routing and cross-system halves stay; the medium (albedo, phase, the
         // multi-scattering series, the ambient tint) is material now.
         { "SkyOcclusionVolume", kCloudRenderer },
         { "PerSampleAtmosphereTransmittance", kCloudPayload },
         { "AerialPerspectiveStartDistance", kCloudPayload },
         { "AerialPerspectiveFadeDistance", kCloudPayload },
         { "LightMarchDistance", kCloudPayload },
         { "LightMarchSamples", kCloudPayload },

         // Shadows on the world. NEITHER GOES THROUGH THE PACKER, and that is the one thing worth
         // knowing about this pair: the shadow map is not part of CloudGpuPayload at all. `CastShadows`
         // is the zero-cost gate the renderer tests before it allocates or dispatches anything, and
         // `ShadowStrength` reaches the GPU through the CONSUMER - CloudShadowUniforms::Params.w in
         // MaterialDeferredLighting - because the map holds the medium's own physical numbers and the
         // artist's dial is applied where the transmittance is reconstructed. Both are read by
         // VolumetricCloudRenderer::GetShadowStrength(), which is the one place the two are combined.
         { "CastShadows", kCloudRenderer },
         { "ShadowStrength", kCloudRenderer },

         // Quality. The two march budgets go through the packer; the BAKE budget does not and cannot -
         // the modelling volume is built on the CPU, before any packing, and its resolution is read by
         // VolumetricCloudRenderer::BuildProceduralParams into
         // Assets::CloudProceduralFieldParams::VolumeSideVoxels. Same category, two different consumers,
         // and that is the distinction the whole field exists to make.
         { "MaxSteps", kCloudPayload },
         { "StopTransmittance", kCloudPayload },
         { "VolumeResolution", kCloudRenderer },

         // Animation - integrated against the timestep by the system that owns it, and handed to the
         // packer as an offset. WIND STAYS ON THE COMPONENT deliberately (the one named divergence from
         // the UE split): the collector may not touch Runtime::ResourceRegistry, and accumulating in the
         // renderer would let two viewports of one scene drift apart.
         { "WindDirection", kCloudSystem },
         { "WindSpeed", kCloudSystem },
    };

    // ------------------------------------------------------------------------------------------------
    // The HERO CLOUD - slot A of the seam, one sculpted body placed by an entity's own transform.
    //
    // ITS FIELDS SPLIT THREE WAYS AND EACH WAY MEANS SOMETHING. `Enabled` and `Volume` are the ECS
    // system's and the renderer's: the first decides whether the instance is COLLECTED at all (which is
    // what makes a disabled hero cloud cost nothing rather than nearly nothing), the second is a handle
    // the renderer resolves through Runtime::CloudModellingService. Everything else is packed, and the
    // packer is its own file rather than CloudPayload.hpp because a hero cloud is a per-frame LIST and
    // the layer is one block.
    //
    // There is no row for a transform here, and that is the point of the component's shape: WHERE the
    // cloud is comes from the entity, so there is no authored position to leave unread.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kHeroPayload = "Desert/Desert/Source/Engine/Graphic/Clouds/CloudAuthoredPayload.hpp";

    constexpr Row kHeroCloudRows[] = {
         { "Enabled", kCloudSystem }, // the zero-cost gate: not collected, so the march's loop is empty
         // THE COLLECTOR AND NOT THE RENDERER, and the row MOVED here in Д23. The handle is resolved
         // through Runtime::CloudModellingService inside VolumetricCloudRenderer.cpp, which is the fuller
         // consumption — but there it is read off a `Graphic::HeroCloudInstance`, one struct removed from
         // the component, and this audit deliberately does not follow a type through a wrapper it cannot
         // see. The collector's own read (`hero.Data.Volume == AssetHandle::Null()` decides whether the
         // body is collected at all) is a real consumption and is written plainly, so that is what the row
         // names. The old row pointed at the renderer and passed for a bad reason: the word "Volume"
         // appears eight times in that file — the noise volume, the atlas volume, the sky-occlusion
         // volume — and none of those is this field.
         { "Volume", kCloudSystem },
         { "Strength", kHeroPayload },
         { "SuppressProceduralField", kHeroPayload },
         { "DetailFactor", kHeroPayload },
         { "DensityFactor", kHeroPayload },
         { "ExtinctionFactor", kHeroPayload },
    };

    // ------------------------------------------------------------------------------------------------
    // The UI canvas. This table is here because of what it caught by NOT being here.
    //
    // UICanvasData::Sprite - "Background Sprite" - was reflected, serialized, shown in Details and read by
    // NOTHING for its whole life. The old ImGui canvas renderer drew it only when handed a SpriteResolver,
    // and the panel that owned it never passed one; then that renderer was deleted and the field had no
    // reader at all. It is fixed (UICanvasRenderer2D draws the full-canvas backdrop before the children),
    // and this table is what keeps a tenth field from joining the component the same way.
    //
    // AND THE ROW SHAPE ALONE WOULD NOT HAVE CAUGHT IT EITHER, which is what Д23 fixed one level up:
    // "Sprite" is also a field of the button, the panel and the image, so UICanvasRenderer2D.cpp mentioned
    // the word on the day the canvas's copy was dead. The rows below now demand an anchored read, and the
    // canvas's background additionally keeps the exact-expression assertion further down the file.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kCanvasRenderer = "Desert/Desert/Source/Engine/UI/UICanvasRenderer2D.cpp";
    constexpr const char* kAnimationSystem = "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp";
    constexpr const char* kCanvasLayout   = "Desert/Desert/Source/Engine/UI/UICanvasLayout.cpp";

    constexpr Row kCanvasRows[] = {
         // The canvas rect and its scale: ResolveCanvas, at the top of the walk.
         { "ScaleMode", kCanvasRenderer },
         { "ReferenceWidth", kCanvasRenderer },
         { "ReferenceHeight", kCanvasRenderer },
         { "MatchWidthHeight", kCanvasRenderer },
         // Screen-space vs billboarded, and the distance scale the billboard uses.
         { "RenderMode", kCanvasRenderer },
         { "WorldScale", kCanvasRenderer },
         // The gate, the backdrop and the notch inset.
         { "Visible", kCanvasRenderer },
         { "Sprite", kCanvasRenderer },
         { "SafeArea", kCanvasRenderer },
         // Which canvas is on top when a view draws several — read by UI::CanvasesInDrawOrder, which is
         // the ordering every drawing host now uses. It is in the LAYOUT file and not the renderer's
         // because deciding which canvases to walk, and in which order, is a query about the scene rather
         // than a step of a walk.
         { "SortOrder", kCanvasLayout },
         // THEME AND ACCESSIBILITY (Ю13). All three are read where the canvas's CanvasStyle is built, at
         // the top of RenderCanvas2D, and from there every element of the canvas resolves through them.
         // The renderer rather than the layout, for the reason the transform trio above gives: a row names
         // the consumer whose absence would make the field do nothing, and nothing about where an element
         // IS depends on which theme it wears.
         { "Theme", kCanvasRenderer },
         { "FontScale", kCanvasRenderer },
         { "HighContrast", kCanvasRenderer },
    };

    // WHERE AN ELEMENT'S COLOURS COME FROM (Ю13). Two fields and one consumer: UICanvasRenderer2D's
    // StyleFor, which is the only place an element is paired with a style. `Source` decides whether the
    // theme is consulted at all and `Style` names which of its styles — a dead row here would be a theme
    // that silently applies to an element that opted out, or an opt-out that silently applies to one that
    // did not, and both look like a correctly drawn UI.
    constexpr Row kStyleRows[] = {
         { "Source", kCanvasRenderer },
         { "Style", kCanvasRenderer },
    };

    // ------------------------------------------------------------------------------------------------
    // SCENE SETTINGS - the one reflected type that is not an ECS component, so it is reached through an
    // accessor (`GetSettings()`) rather than through a wrapper's `Data`.
    //
    // Most of it funnels through SceneRenderer::SetSceneData, which is where the scene's authored
    // rendering policy becomes the frame's. The exceptions are named individually because WHERE a setting
    // is consumed is the interesting half of the row: physics reads its own two, and the splash trio is
    // the RUNTIME's - it is the only thing in this file consumed by the shipping player and by neither the
    // editor nor the renderer.
    //
    // THE TWO EDITOR-ONLY AIDS USED TO BE THE OTHER EXCEPTION, and their disappearance from this table is
    // the point of К2. `ShowGrid` and `ShowColliders` were rows pointing at EditorGridPass and
    // EditorColliderPass - correct rows, about fields that should never have been in a scene file at all.
    // A consumer census answers "does anything read this?"; it cannot answer "should this be here?", and
    // both fields passed it for as long as they existed. Desert/Tests/Engine/SceneDebugFields is the
    // census that asks the second question.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kSceneRenderer = "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp";
    constexpr const char* kPhysicsSystem = "Desert/Desert/Source/Engine/ECS/System/PhysicsECSSystem.hpp";
    constexpr const char* kRuntimeLayer  = "Runtime/Source/RuntimeLayer.cpp";

    constexpr Row kSceneSettingsRows[] = {
         { "RenderingPath", kSceneRenderer },
         { "EnableSSAO", kSceneRenderer },
         { "GlobalIllumination", kSceneRenderer },
         { "GIIntensity", kSceneRenderer },
         { "EnableSSR", kSceneRenderer },
         { "SSRIntensity", kSceneRenderer },
         { "SSRMaxDistance", kSceneRenderer },
         { "EnableShadows", kSceneRenderer },
         { "ShadowBias", kSceneRenderer },
         { "CascadeSplitLambda", kSceneRenderer },
         { "Tonemapper", kSceneRenderer },
         { "Exposure", kSceneRenderer },
         { "Gamma", kSceneRenderer },
         { "WhitePoint", kSceneRenderer },
         { "AutoExposure", kSceneRenderer },
         { "AutoExposureKey", kSceneRenderer },
         { "AutoExposureSpeed", kSceneRenderer },
         { "AutoExposureMin", kSceneRenderer },
         { "AutoExposureMax", kSceneRenderer },
         { "EnableBloom", kSceneRenderer },
         { "BloomThreshold", kSceneRenderer },
         { "BloomIntensity", kSceneRenderer },
         { "LensDispersion", kSceneRenderer },
         { "EnableLensFlare", kSceneRenderer },
         { "LensFlareIntensity", kSceneRenderer },
         { "LensFlareTint", kSceneRenderer },
         { "LensFlareThreshold", kSceneRenderer },
         { "LensFlareGhostCount", kSceneRenderer },
         { "LensFlareGhostSpacing", kSceneRenderer },
         { "LensFlareGhostSizeNear", kSceneRenderer },
         { "LensFlareGhostSizeFar", kSceneRenderer },
         { "LensFlareGhostTintInner", kSceneRenderer },
         { "LensFlareGhostTintOuter", kSceneRenderer },
         { "LensFlareHaloIntensity", kSceneRenderer },
         { "LensFlareHaloRadius", kSceneRenderer },
         { "LensFlareStreakIntensity", kSceneRenderer },
         { "LensFlareStreakLength", kSceneRenderer },
         { "LensFlareStreakAngle", kSceneRenderer },
         { "LensFlareChromaShift", kSceneRenderer },

         // THE FIVE MACHINE-QUALITY ROWS THAT USED TO SIT HERE ARE GONE WITH THE FIELDS (К3): AA, MeshLOD,
         // TextureFilterMode, Anisotropy and CloudQualityTier. Every one was a CORRECT row about a field
         // that should never have been in a level file — the same thing К2's ShowGrid/ShowColliders rows
         // were, and the same lesson: a consumer census answers "does anything read this?" and cannot
         // answer "should this be here?". They live in Common::Settings::MachineSettings now, and
         // Desert/Tests/Engine/ConfigOwnership is the census that asks the second question about them.

         // The eight debug rows that used to sit here - ShowGrid, ShowColliders, ShowBoundingBoxes,
         // BoundingBoxColor, BoundingBoxLineWidth, WireframeMode, ShadowDebug, DeferredDebug - are gone
         // with the fields (К2). They were never level data: they said what a VIEWPORT was drawing on top
         // of the world, and 55 of 80 scenes shipped `ShowColliders: true` through git as a result. They
         // live in Graphic::DebugViewState now, one per SceneRenderer, pushed in from
         // Editor::EditorPreferences and serialized nowhere. Desert/Tests/Engine/SceneDebugFields is the
         // census that keeps them out - of this struct and of every .desce on disk - and it derives its
         // list from DebugViewState's own declaration rather than from a second hand-written table.

         { "Gravity", kPhysicsSystem },
         // "PauseSimulation" had a DEAD row here. Д26 deleted the field rather than wiring it: the
         // editor's transport owns pausing (runtime state, not scene data), and sixteen probe scenes had
         // authored `true` and got nothing — the misleading-knob defect itself.

         // The three Wind rows are gone with their fields (Г26). They were WIRED to SceneRenderer, and
         // that row was TRUE and USELESS: BeginScene did read all three into a WindEnv, and nothing ever
         // read the WindEnv, so the census was satisfied one link before the frame. See the note on
         // LandscapeMaterialData's Frame anchors below, which is this suite's answer to that shape.

         // The shipping player's, and nothing else's.
         { "SplashSprite", kRuntimeLayer },
         { "SplashDuration", kRuntimeLayer },
         { "SplashFade", kRuntimeLayer },
    };

    // ------------------------------------------------------------------------------------------------
    // The scene's own components: camera, terrain, the three lights, particles, skybox, physics, audio.
    //
    // THE DIRECTIONAL LIGHT IS THE ONE TO READ TWICE. Its ten fields split between two consumers and the
    // split is physical: Colour and Intensity are surface illumination, collected with every other light
    // by Scene::CollectRenderData; the eight Atmosphere/Light-Shafts fields belong to whichever light the
    // sky elected as its sun, so the SKY's collector is what reads them, and reading them anywhere else
    // would let an unmarked light in a corner of the scene tint the sky.
    // ------------------------------------------------------------------------------------------------

    constexpr const char* kScene      = "Desert/Desert/Source/Engine/Core/Scene.cpp";
    constexpr const char* kLandscape  = "Desert/Desert/Source/Engine/ECS/System/LandscapeECSSystem.cpp";
    constexpr const char* kPointLight = "Desert/Desert/Source/Engine/ECS/System/PointLightSystem.hpp";
    constexpr const char* kSpotLight  = "Desert/Desert/Source/Engine/ECS/System/SpotLightSystem.hpp";
    constexpr const char* kLightGizmo = "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp";
    constexpr const char* kParticles =
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp";
    constexpr const char* kAudioSystem = "Desert/Desert/Source/Engine/ECS/System/AudioECSSystem.hpp";

    constexpr Row kCameraRows[] = {
         { "IsMainCamera", kScene }, // which camera the scene renders through
         { "FOV", kScene },
         { "Near", kScene },
         { "Far", kScene },
    };

    // The one surface text every landscape in this engine is shaded by — Terrain.shader (forward) and
    // TerrainGBuffer.shader (deferred) both include it. A layer mode that LandscapeECSSystem packs into the
    // draw command has to be READ here, or the mode is a combo box that moves nothing.
    constexpr const char* kTerrainShader = "Editor/Resources/Shaders/Programs/Terrain/TerrainSurface.glslh";

    constexpr Row kLandscapeMaterialRows[] = {
         { "Material", kLandscape },
         // The three layer modes state BOTH ends: the C++ that packs the enum, and the slot the shader
         // must read it out of. Two of the three have been dead in this exact way — GrassMode until Г25,
         // RockMode until Г26 — with this census green throughout.
         { "GrassMode", kLandscape, nullptr, nullptr, kTerrainShader, "u_T.LayerModes.x" },
         { "RockMode", kLandscape, nullptr, nullptr, kTerrainShader, "u_T.LayerModes.y" },
         { "SnowMode", kLandscape, nullptr, nullptr, kTerrainShader, "u_T.LayerModes.z" },
    };

    constexpr Row kDirLightRows[] = {
         { "Color", kScene },
         { "Intensity", kScene },
         { "AtmosphereSunLight", kCollector },
         { "AtmosphereSunLightIndex", kCollector },
         { "AffectedByAtmosphereTransmittance", kCollector },
         { "LightShaftBloom", kCollector },
         { "BloomScale", kCollector },
         { "BloomThreshold", kCollector },
         { "BloomMaxBrightness", kCollector },
         { "BloomTint", kCollector },
    };

    constexpr Row kPointLightRows[] = {
         { "Color", kPointLight },
         { "Intensity", kPointLight },
         { "Radius", kPointLight },
         { "MinRadius", kPointLight },
         { "Falloff", kPointLight },
         // An EDITOR-ONLY visualisation flag, deliberately: it draws the radius gizmo and reaches no
         // shader at all, so the gizmo renderer is the honest consumer to name.
         { "ShowRadius", kLightGizmo },
    };

    constexpr Row kSpotLightRows[] = {
         { "Color", kSpotLight },          { "Intensity", kSpotLight },      { "Range", kSpotLight },
         { "InnerConeAngle", kSpotLight }, { "OuterConeAngle", kSpotLight }, { "Falloff", kSpotLight },
         { "ShowCone", kLightGizmo }, // editor-only, same as ShowRadius above
    };

    constexpr Row kParticleRows[] = {
         { "Enabled", kParticles },
         { "MaxParticles", kParticles },
         { "SpawnRate", kParticles },
         { "Looping", kParticles },
         // WIRED BY Д26, after a lifetime as a dead row: the renderer folds it into the sim push
         // (Counts.w), and the compute pass keeps a local-mode particle's offset from the emitter and
         // rebases it on the current emitter position — so "Simulate In World" off makes the system RIDE
         // a moving emitter instead of trailing behind it, which is what the tooltip promised all along.
         { "WorldSpace", kParticles },
         { "Lifetime", kParticles },
         { "LifetimeVariance", kParticles },
         { "StartSpeed", kParticles },
         { "SpeedVariance", kParticles },
         { "Direction", kParticles },
         { "ConeAngle", kParticles },
         { "Gravity", kParticles },
         { "StartSize", kParticles },
         { "SizeCurvePower", kParticles },
         { "EndSize", kParticles },
         { "StartColor", kParticles },
         { "EndColor", kParticles },
         { "StartAlpha", kParticles },
         { "EndAlpha", kParticles },
         { "Blend", kParticles },
    };

    // All three reach the frame the same way: SkyboxECSSystem packs them into a Graphic::SkyLook on the
    // SkyboxCommand, and every reader of the environment cubes applies it where it samples them. Naming the
    // COLLECTOR is right for all three — it is the file that would have to change for a knob to stop being read.
    constexpr Row kSkyboxRows[] = {
         { "SkyboxHandle", kCollector },
         { "Intensity", kCollector },
         { "Rotation", kCollector },
         { "Tint", kCollector },
    };

    constexpr Row kColliderRows[] = {
         { "Shape", kPhysicsSystem },
         { "HalfExtents", kPhysicsSystem },
         { "Radius", kPhysicsSystem },
         { "HalfHeight", kPhysicsSystem },
    };

    constexpr Row kRigidBodyRows[] = {
         { "Type", kPhysicsSystem },
         { "Mass", kPhysicsSystem },
         { "Friction", kPhysicsSystem },
         { "Restitution", kPhysicsSystem },
    };

    constexpr Row kCharacterControllerRows[] = {
         { "Radius", kPhysicsSystem },
         { "Height", kPhysicsSystem },
         { "MaxSlopeDeg", kPhysicsSystem },
         { "Gravity", kPhysicsSystem },
    };

    constexpr Row kAudioRows[] = {
         { "Clip", kAudioSystem },     { "Volume", kAudioSystem },  { "Loop", kAudioSystem },
         { "AutoPlay", kAudioSystem }, { "Spatial", kAudioSystem },
    };

    // ------------------------------------------------------------------------------------------------
    // THE TWENTY UI COMPONENTS THAT HAD NO TABLE. This is the bulk of what Д23 added, and it is the half
    // of the engine where a dead setting is hardest to see: a UI field that nothing reads produces a
    // perfectly ordinary-looking panel, exactly as the canvas's own background did for its whole life.
    //
    // Two consumers between them. UICanvasLayout.cpp answers WHERE an element is - anchors, offsets,
    // fitters, the layout groups and the two inherited axes; UICanvasRenderer2D.cpp answers WHAT is drawn
    // and what the pointer hits. A field's row names whichever of the two actually reads it, and the
    // split is not cosmetic: `Visibility` is read by the layout walk (a collapsed element gives its slot
    // back) while `HitTest` is read by the hit walk, and swapping them would name a file that never sees
    // the value.
    // ------------------------------------------------------------------------------------------------

    constexpr Row kLayoutRows[] = {
         { "AnchorMin", kCanvasLayout },
         { "AnchorMax", kCanvasLayout },
         { "OffsetMin", kCanvasLayout },
         { "OffsetMax", kCanvasLayout },
         { "CustomMinimumSize", kCanvasLayout },
         // THE DAY ARRIVED. "Pivot" had a DEAD row here, then no row at all: Д26 deleted the field
         // because the rect was resolved from anchors and offsets alone and nothing in this UI rotated
         // or scaled an element about a point, and the note left behind said the field returns with its
         // consumer. Ю8 is that consumer, and all three rows below are WIRED to it.
         //
         // The renderer is named rather than the layout, and the split matters here exactly as it does
         // for Visibility/HitTest: these three are read by UICanvasRenderer2D.cpp, which turns the
         // element's geometry and undoes the pointer through the same matrix. UICanvasLayout.cpp reads
         // them too (the editor's pick has to agree), but a row names the consumer whose absence would
         // make the field do nothing, and that is the renderer.
         { "Rotation", kCanvasRenderer },
         { "Scale", kCanvasRenderer },
         { "Pivot", kCanvasRenderer },
         { "ClipContents", kCanvasRenderer },
         { "Visibility", kCanvasLayout },
         { "HitTest", kCanvasRenderer },
         { "AspectRatio", kCanvasLayout },
         { "AspectMode", kCanvasLayout },
         { "FlexGrow", kCanvasLayout },
         { "FitWidth", kCanvasLayout },
         { "FitHeight", kCanvasLayout },
    };

    constexpr Row kLayoutGroupRows[] = {
         { "Type", kCanvasLayout },         { "Padding", kCanvasLayout },  { "Spacing", kCanvasLayout },
         { "StretchCross", kCanvasLayout }, { "CellSize", kCanvasLayout }, { "Columns", kCanvasLayout },
    };

    constexpr Row kPanelRows[] = {
         { "Color", kCanvasRenderer },
         { "Opacity", kCanvasRenderer },
         { "CornerRadius", kCanvasRenderer },
         { "BackdropBlur", kCanvasRenderer },
         { "Sprite", kCanvasRenderer },
         { "SpriteBorder", kCanvasRenderer },
         { "Video", kCanvasRenderer },
         { "Circle", kCanvasRenderer },
         { "RingWidth", kCanvasRenderer },
         { "RingColorA", kCanvasRenderer },
         { "RingColorB", kCanvasRenderer },
         { "Pulse", kCanvasRenderer },
         { "PulseSpeed", kCanvasRenderer },
         { "PulseMin", kCanvasRenderer },
         { "UseGradient", kCanvasRenderer },
         { "GradientColor", kCanvasRenderer },
         { "BorderWidth", kCanvasRenderer },
         { "BorderColor", kCanvasRenderer },
         { "Shadow", kCanvasRenderer },
         { "ShadowColor", kCanvasRenderer },
         { "ShadowOffset", kCanvasRenderer },
         { "Glow", kCanvasRenderer },
         { "GlowColor", kCanvasRenderer },
         { "GlowSize", kCanvasRenderer },
         // The UI-material slot is read by the canvas walk like every other fill field; what it
         // resolves to is Render2D's business (Graphic/Render2D/UIMaterialCache.cpp).
         { "Material", kCanvasRenderer },
    };

    constexpr Row kButtonRows[] = {
         { "NormalColor", kCanvasRenderer },    { "HoverColor", kCanvasRenderer },
         { "PressedColor", kCanvasRenderer },   { "Action", kCanvasRenderer },
         { "OnClickMessage", kCanvasRenderer }, { "Sprite", kCanvasRenderer },
         { "HoverSprite", kCanvasRenderer },    { "PressedSprite", kCanvasRenderer },
         { "SpriteBorder", kCanvasRenderer },   { "Selected", kCanvasRenderer },
         { "SelectedColor", kCanvasRenderer },  { "SelectedAccent", kCanvasRenderer },
         { "Disabled", kCanvasRenderer },       { "DisabledColor", kCanvasRenderer },
    };

    constexpr Row kTextRows[] = {
         { "Text", kCanvasRenderer },         { "FontSize", kCanvasRenderer },
         { "Font", kCanvasRenderer },         { "Color", kCanvasRenderer },
         { "Align", kCanvasRenderer },        { "VerticalAlign", kCanvasRenderer },
         { "Wrap", kCanvasRenderer },         { "LineSpacing", kCanvasRenderer },
         { "AutoSize", kCanvasRenderer },     { "MinFontSize", kCanvasRenderer },
         { "Overflow", kCanvasRenderer },     { "RichText", kCanvasRenderer },
         { "Marquee", kCanvasRenderer },      { "MarqueeSpeed", kCanvasRenderer },
         { "Shadow", kCanvasRenderer },       { "ShadowColor", kCanvasRenderer },
         { "ShadowOffset", kCanvasRenderer }, { "Outline", kCanvasRenderer },
         { "OutlineColor", kCanvasRenderer },
    };

    constexpr Row kImageRows[] = {
         { "Sprite", kCanvasRenderer },
         { "Tint", kCanvasRenderer },
         { "Opacity", kCanvasRenderer },
         { "SpriteBorder", kCanvasRenderer },
    };

    constexpr Row kIconRows[] = {
         { "Icon", kCanvasRenderer },
         { "Color", kCanvasRenderer },
         { "Scale", kCanvasRenderer },
    };

    // Ю16. All four are read in the canvas walk: ScenePath and ResolutionScale by ResolveRenderTexture,
    // which is the only place that knows how many texels the quad shows, and Tint/Opacity at the draw
    // site. The BACKEND then reads the path again out of the request — one value, passed, not copied.
    // Every field is copied into the entity's live TwoBoneIKControl by SyncSkeletalControls, which is the
    // ONE place the authored data crosses into the solver. There is no Frame anchor here and the reason is
    // worth stating: the value's destination is a bone transform in a pose, not a shader uniform — the
    // frame end of this chain is pinned by `Tests/Engine/BoneControlContract` (the skinning matrices the
    // GPU sees carry the solve) and by the shots in Docs/Animation/Shots/A3.
    constexpr Row kTwoBoneIKRows[] = {
         { "EndBone", kAnimationSystem },
         { "Goal", kAnimationSystem },
         { "PoleTarget", kAnimationSystem },
         { "Alpha", kAnimationSystem },
    };

    // The rig slot is ONE authored value, and the consumer is the per-frame sync that turns the handle
    // into the Animator's Rig stage. Deliberately no Alpha beside it: ControlRigStage applies its
    // overrides at 1.0 and says why, so a weight here would be a field with no reader.
    constexpr Row kControlRigRows[] = {
         { "Rig", kAnimationSystem },
    };

    // The retarget slot is ONE authored value, and the consumer is the per-frame sync that turns the
    // handle into the Animator's source rig. Deliberately no source-rig handle beside it: the pair lives
    // in the file, so there is no second value here that could disagree with it.
    constexpr Row kRetargetRows[] = {
         { "Retarget", kAnimationSystem },
    };

    constexpr Row kRenderTextureRows[] = {
         { "ScenePath", kCanvasRenderer },
         { "Tint", kCanvasRenderer },
         { "Opacity", kCanvasRenderer },
         { "ResolutionScale", kCanvasRenderer },
    };

    constexpr Row kProgressBarRows[] = {
         { "Value", kCanvasRenderer },
         { "Background", kCanvasRenderer },
         { "Fill", kCanvasRenderer },
         { "CornerRadius", kCanvasRenderer },
    };

    constexpr Row kToggleRows[] = {
         { "Value", kCanvasRenderer },
         { "BoxColor", kCanvasRenderer },
         { "CheckColor", kCanvasRenderer },
         { "CornerRadius", kCanvasRenderer },
    };

    constexpr Row kSliderRows[] = {
         { "Value", kCanvasRenderer },      { "MinValue", kCanvasRenderer },  { "MaxValue", kCanvasRenderer },
         { "TrackColor", kCanvasRenderer }, { "FillColor", kCanvasRenderer }, { "HandleColor", kCanvasRenderer },
    };

    constexpr Row kScrollViewRows[] = {
         { "ScrollY", kCanvasRenderer },        { "ContentHeight", kCanvasRenderer },
         { "Background", kCanvasRenderer },     { "ShowScrollbar", kCanvasRenderer },
         { "ScrollbarColor", kCanvasRenderer },
    };

    constexpr Row kListViewRows[] = {
         { "ScrollY", kCanvasRenderer },        { "ItemHeight", kCanvasRenderer },
         { "Spacing", kCanvasRenderer },        { "Overscan", kCanvasRenderer },
         { "Background", kCanvasRenderer },     { "ShowScrollbar", kCanvasRenderer },
         { "ScrollbarColor", kCanvasRenderer },
    };

    constexpr Row kInputFieldRows[] = {
         { "Text", kCanvasRenderer },
         { "Placeholder", kCanvasRenderer },
         { "FontSize", kCanvasRenderer },
         { "TextColor", kCanvasRenderer },
         { "PlaceholderColor", kCanvasRenderer },
         { "Background", kCanvasRenderer },
         { "FocusColor", kCanvasRenderer },
         { "CornerRadius", kCanvasRenderer },
    };

    constexpr Row kDropdownRows[] = {
         { "Options", kCanvasRenderer },   { "SelectedIndex", kCanvasRenderer }, { "Open", kCanvasRenderer },
         { "FontSize", kCanvasRenderer },  { "Background", kCanvasRenderer },    { "TextColor", kCanvasRenderer },
         { "Highlight", kCanvasRenderer }, { "CornerRadius", kCanvasRenderer },
    };

    constexpr Row kTweenRows[] = {
         { "Property", kCanvasRenderer }, { "From", kCanvasRenderer },    { "To", kCanvasRenderer },
         { "Duration", kCanvasRenderer }, { "Delay", kCanvasRenderer },   { "Easing", kCanvasRenderer },
         { "Loop", kCanvasRenderer },     { "Playing", kCanvasRenderer }, { "RewindOnHide", kCanvasRenderer },
    };

    constexpr Row kBindingRows[] = {
         { "Key", kCanvasRenderer }, { "Target", kCanvasRenderer },
         // `Format` was here and is GONE (Ю15). It held a printf format an author typed in the Details
         // panel and the canvas handed to std::snprintf with a double, and it formatted every bound number
         // in the C locale whatever language the reader was in. Its job is the string table's now.
    };

    constexpr Row kScreenRows[] = {
         { "Name", kCanvasRenderer },
    };

    constexpr Row kScreenStackRows[] = {
         { "InitialScreen", kCanvasRenderer },
         { "TransitionTime", kCanvasRenderer },
         { "SlidePx", kCanvasRenderer },
         { "Easing", kCanvasRenderer },
    };

    constexpr Row kPointerEventsRows[] = {
         { "OnEnterMessage", kCanvasRenderer }, { "OnExitMessage", kCanvasRenderer },
         { "OnDownMessage", kCanvasRenderer },  { "OnUpMessage", kCanvasRenderer },
         { "Phase", kCanvasRenderer },          { "StopPropagation", kCanvasRenderer },
    };

    constexpr Row kDraggableRows[] = {
         { "Payload", kCanvasRenderer },
         { "GhostOpacity", kCanvasRenderer },
    };

    constexpr Row kDropTargetRows[] = {
         { "Accepts", kCanvasRenderer },
         { "OnDropMessage", kCanvasRenderer },
         { "HighlightColor", kCanvasRenderer },
    };

    // Overlays (Ю12). Most of the policy is read by the state machine; the scrim is read by the walk that
    // draws it, because the scrim is also the election that makes a modal modal.
    constexpr const char* kOverlay = "Desert/Desert/Source/Engine/UI/UIOverlay.cpp";

    constexpr Row kOverlayRows[] = {
         { "Kind", kOverlay },
         { "Name", kOverlay },
         // Placement: the gap from the origin, the hover delay before it opens, and whether a tooltip
         // rides the cursor or is pinned to the element it describes.
         { "Gap", kOverlay },
         { "OpenDelay", kOverlay },
         { "FollowPointer", kOverlay },
         // Dismissal.
         { "CloseOnEscape", kOverlay },
         { "CloseOnClickOutside", kOverlay },
         // The modal's dim, drawn AND elected by the canvas walk.
         { "ScrimColor", kCanvasRenderer },
         { "ScrimOpacity", kCanvasRenderer },
         // The toast queue's clock and its bound.
         { "ToastLifetime", kOverlay },
         { "ToastSlots", kOverlay },
    };

    constexpr Row kOverlayTriggerRows[] = {
         { "Overlay", kOverlay },
         { "On", kOverlay },
         { "Text", kOverlay },
    };

    // ------------------------------------------------------------------------------------------------
    // THE CENSUS. Thirty-nine reflected types, thirty-nine entries; adding a fortieth fails
    // `EveryReflectedTypeIsUnderThisCensus` before it can reach a Details panel with no reader.
    // ------------------------------------------------------------------------------------------------

#define CENSUS_ROWS( rows ) ( rows ), std::size( rows )

    constexpr Census kCensus[] = {
         { "SceneSettings", nullptr, "GetSettings", CENSUS_ROWS( kSceneSettingsRows ) },
         { "SkyAtmosphereData", "SkyAtmosphereComponent", nullptr, CENSUS_ROWS( kSkyRows ) },
         { "ExponentialHeightFogData", "ExponentialHeightFogComponent", nullptr, CENSUS_ROWS( kFogRows ) },
         { "VolumetricCloudData", "VolumetricCloudComponent", nullptr, CENSUS_ROWS( kCloudRows ) },
         { "HeroCloudData", "HeroCloudComponent", nullptr, CENSUS_ROWS( kHeroCloudRows ) },

         { "CameraData", "CameraComponent", nullptr, CENSUS_ROWS( kCameraRows ) },
         { "LandscapeMaterialData", "LandscapeMaterialComponent", nullptr, CENSUS_ROWS( kLandscapeMaterialRows ) },
         // NOT `DirectionalLightComponent`. The wrapper dropped the "al", and a census that guessed the
         // spelling would have found no receivers at all and called ten live fields dead.
         { "DirectionalLightData", "DirectionLightComponent", nullptr, CENSUS_ROWS( kDirLightRows ) },
         { "PointLightData", "PointLightComponent", nullptr, CENSUS_ROWS( kPointLightRows ) },
         { "SpotLightData", "SpotLightComponent", nullptr, CENSUS_ROWS( kSpotLightRows ) },
         { "ParticleEmitterData", "ParticleEmitterComponent", nullptr, CENSUS_ROWS( kParticleRows ) },
         // The one component whose reflected type IS the component: there is no `Data` member to hop
         // through, so it anchors on its own name.
         { "SkyboxComponent", nullptr, nullptr, CENSUS_ROWS( kSkyboxRows ) },
         { "ColliderData", "ColliderComponent", nullptr, CENSUS_ROWS( kColliderRows ) },
         { "RigidBodyData", "RigidBodyComponent", nullptr, CENSUS_ROWS( kRigidBodyRows ) },
         { "CharacterControllerData", "CharacterControllerComponent", nullptr,
           CENSUS_ROWS( kCharacterControllerRows ) },
         { "AudioSourceData", "AudioSourceComponent", nullptr, CENSUS_ROWS( kAudioRows ) },

         { "UICanvasData", "UICanvasComponent", nullptr, CENSUS_ROWS( kCanvasRows ) },
         { "UIStyleData", "UIStyleComponent", nullptr, CENSUS_ROWS( kStyleRows ) },
         { "UILayoutData", "UILayoutComponent", nullptr, CENSUS_ROWS( kLayoutRows ) },
         { "UILayoutGroupData", "UILayoutGroupComponent", nullptr, CENSUS_ROWS( kLayoutGroupRows ) },
         { "UIPanelData", "UIPanelComponent", nullptr, CENSUS_ROWS( kPanelRows ) },
         { "UIButtonData", "UIButtonComponent", nullptr, CENSUS_ROWS( kButtonRows ) },
         // ...and the other exception to the wrapper's spelling: the 2D suffix.
         { "UITextData", "UITextComponent2D", nullptr, CENSUS_ROWS( kTextRows ) },
         { "UIImageData", "UIImageComponent", nullptr, CENSUS_ROWS( kImageRows ) },
         { "UIIconData", "UIIconComponent", nullptr, CENSUS_ROWS( kIconRows ) },
         { "UIRenderTextureData", "UIRenderTextureComponent", nullptr, CENSUS_ROWS( kRenderTextureRows ) },
         { "TwoBoneIKData", "TwoBoneIKComponent", nullptr, CENSUS_ROWS( kTwoBoneIKRows ) },
         { "ControlRigData", "ControlRigComponent", nullptr, CENSUS_ROWS( kControlRigRows ) },
         { "RetargetData", "RetargetComponent", nullptr, CENSUS_ROWS( kRetargetRows ) },
         { "UIProgressBarData", "UIProgressBarComponent", nullptr, CENSUS_ROWS( kProgressBarRows ) },
         { "UIToggleData", "UIToggleComponent", nullptr, CENSUS_ROWS( kToggleRows ) },
         { "UISliderData", "UISliderComponent", nullptr, CENSUS_ROWS( kSliderRows ) },
         { "UIScrollViewData", "UIScrollViewComponent", nullptr, CENSUS_ROWS( kScrollViewRows ) },
         { "UIListViewData", "UIListViewComponent", nullptr, CENSUS_ROWS( kListViewRows ) },
         { "UIInputFieldData", "UIInputFieldComponent", nullptr, CENSUS_ROWS( kInputFieldRows ) },
         { "UIDropdownData", "UIDropdownComponent", nullptr, CENSUS_ROWS( kDropdownRows ) },
         { "UITweenData", "UITweenComponent", nullptr, CENSUS_ROWS( kTweenRows ) },
         { "UIBindingData", "UIBindingComponent", nullptr, CENSUS_ROWS( kBindingRows ) },
         { "UIScreenData", "UIScreenComponent", nullptr, CENSUS_ROWS( kScreenRows ) },
         { "UIScreenStackData", "UIScreenStackComponent", nullptr, CENSUS_ROWS( kScreenStackRows ) },
         { "UIPointerEventsData", "UIPointerEventsComponent", nullptr, CENSUS_ROWS( kPointerEventsRows ) },
         { "UIDraggableData", "UIDraggableComponent", nullptr, CENSUS_ROWS( kDraggableRows ) },
         { "UIDropTargetData", "UIDropTargetComponent", nullptr, CENSUS_ROWS( kDropTargetRows ) },
         { "UIOverlayData", "UIOverlayComponent", nullptr, CENSUS_ROWS( kOverlayRows ) },
         { "UIOverlayTriggerData", "UIOverlayTriggerComponent", nullptr, CENSUS_ROWS( kOverlayTriggerRows ) },
    };

#undef CENSUS_ROWS

    // ------------------------------------------------------------------------------------------------
    // KNOWN DEBT. Every setting that is exposed and read by NOTHING, one line each, by name.
    //
    // The contract (section 1.3) forbids these outright, so this list is a defect register and not a
    // permission: it exists because the census that FINDS them is worth more than the census that would
    // have to be weakened to stay green. Repairing one means deleting its line here and pointing its row
    // at the reader (or deleting the field outright), and the equality below makes both directions - a
    // new dead setting, and a repaired one - a reviewable edit.
    //
    // THE REGISTER IS EMPTY, and that is a measurement rather than an aspiration: Д23 found three
    // (PauseSimulation, the emitter's WorldSpace, the UI Pivot) and Д26 retired all three - one wired,
    // two deleted with their fields. The machinery stays, because the register's value was never the
    // three lines; it is that the NEXT dead setting cannot join a component without a row here saying so.
    // ------------------------------------------------------------------------------------------------

    struct DeadSetting
    {
        const char* Type;
        const char* Field;
    };

    constexpr std::array<DeadSetting, 0> kKnownDeadSettings{};

    // The repository root, found by walking up from wherever the test binary was started - the same
    // approach the font-baker test uses, so neither has to be run from one exact directory.
    std::string RepoRoot()
    {
        // Starts at "./" rather than "": an empty string is this function's "not found", and the root is
        // very often the directory the test was started in.
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // `Foo.cpp` -> `Foo.hpp` and back. Returns the input unchanged for anything else, and a path that
    // does not exist simply reads as empty.
    std::string SiblingOf( const std::string& path )
    {
        if ( path.size() > 4 && path.compare( path.size() - 4, 4, ".cpp" ) == 0 )
            return path.substr( 0, path.size() - 4 ) + ".hpp";
        if ( path.size() > 4 && path.compare( path.size() - 4, 4, ".hpp" ) == 0 )
            return path.substr( 0, path.size() - 4 ) + ".cpp";
        return path;
    }

    const TypeInfo& Type( const char* name )
    {
        const TypeInfo* t = ReflectionRegistry::Get().Find( name );
        EXPECT_NE( t, nullptr ) << name << " is not registered - the codegen did not run";
        return *t;
    }

    std::vector<std::string> AnchorsOf( const Census& c )
    {
        std::vector<std::string> anchors{ c.Type };
        if ( c.Component )
            anchors.emplace_back( c.Component );
        if ( c.Accessor )
            anchors.emplace_back( c.Accessor );
        return anchors;
    }

    // Every reflected field appears in the table exactly once, and every table row names a real field.
    void CheckTableCoversTypeExactly( const TypeInfo& type, const Row* rows, std::size_t count )
    {
        for ( const FieldInfo& f : type.Fields )
        {
            const std::ptrdiff_t hits =
                 std::count_if( rows, rows + count, [&f]( const Row& r ) { return f.Name == r.Field; } );
            EXPECT_EQ( hits, 1 ) << type.Name << "::" << f.Name
                                 << " has no consumer row (or more than one). Every exposed setting must "
                                    "name either the code that reads it, the task that will, or the "
                                    "reason nothing does.";
        }

        for ( const Row* r = rows; r != rows + count; ++r )
        {
            const bool known = std::any_of( type.Fields.begin(), type.Fields.end(),
                                            [r]( const FieldInfo& f ) { return f.Name == r->Field; } );
            EXPECT_TRUE( known ) << r->Field << " is listed as consumed but is not a field of " << type.Name
                                 << " - a stale row outliving the field it described";

            const int kinds = ( r->Where != nullptr ) + ( r->Task != nullptr ) + ( r->Dead != nullptr );
            EXPECT_EQ( kinds, 1 ) << type.Name << "::" << r->Field
                                  << " must name exactly one of: a consumer file, an owing task, or the "
                                     "reason it is dead";
        }

        EXPECT_EQ( count, type.Fields.size() );
    }
} // namespace

TEST( SettingConsumers, EveryReflectedTypeIsUnderThisCensus )
{
    const auto& all = ReflectionRegistry::Get().All();

    for ( const auto& [name, info] : all )
    {
        const bool covered = std::any_of( std::begin( kCensus ), std::end( kCensus ),
                                          [&name]( const Census& c ) { return name == c.Type; } );
        EXPECT_TRUE( covered ) << name
                               << " is a reflected type with no consumer table. Every field it exposes "
                                  "reaches a Details panel; add a table naming who reads each one.";
    }

    for ( const Census& c : kCensus )
        EXPECT_NE( ReflectionRegistry::Get().Find( c.Type ), nullptr )
             << c.Type << " has a consumer table but is no longer reflected - a stale census entry";

    // The count is pinned as well as the membership, because the two fail differently: a type that loses
    // its REFLECT() drops out of `all` silently, and only the number says so.
    EXPECT_EQ( all.size(), std::size( kCensus ) );
    // 37 -> 38 with Ю13's UIStyleData, -> 39 with Ю12's UIOverlayData and UIOverlayTriggerData. Each row
    // is in kCensus above and every field of each is WIRED, so the number moved because the register did,
    // which is the only reason this literal may ever be edited. NEITHER BRANCH'S NUMBER IS RIGHT ALONE:
    // 38 and 39 are each correct against dev and both are wrong here, which is what the register is for.
    // -> 41 with Ю16's UIRenderTextureData. Its four fields are WIRED, all four to the canvas walk:
    // ScenePath and ResolutionScale in ResolveRenderTexture, Tint and Opacity at the draw site.
    // -> 42 with Ю17's UIListViewData. Its seven fields are WIRED, all seven to the canvas walk:
    // ItemHeight/Spacing/Overscan/ScrollY solve the window, Background and ScrollbarColor are drawn
    // through the ScrollView style slots, ShowScrollbar gates the thumb.
    // -> 42 with A3's TwoBoneIKData. Its four fields are WIRED to AnimationECSSystem::SyncSkeletalControls,
    // which is the only place authored IK data becomes a live solver's input.
    //
    //   BOTH SIDES SAID 42 AND BOTH WERE RIGHT ABOUT THEIR OWN HEAD, so the merge is 43 — read off a
    //   run, never added up. Ю17 and А3 each added ONE reflected type to a tree that had 41; neither
    //   could see the other. This census now joins PointerOwnership as a register whose TOTAL cannot be
    //   carried across a merge while its DELTA can, which is the rule six consecutive integrations
    //   established and this is the seventh.
    //
    //   AND THE PARAGRAPH ABOVE WAS ONCE WRITTEN WHILE THE LINE BELOW IT STILL SAID 42. The А3 merge
    //   reasoned its way to 43 in prose and left the literal alone, because the number was read off a
    //   run AFTER the file had already been staged — and a merge commits the INDEX. So the commit
    //   carried 42 while the working tree carried 43, and the full sweep passed because a sweep runs
    //   the WORKING TREE, not the commit: a green sweep certified a tree that was not in history.
    //   Caught before the push, by a later merge refusing to run on a dirty tree, and repaired by
    //   amending. А4 then reported it independently from its own base, which still had the old value.
    //
    //   Two lessons, both paid for: the prose gets corrected and the copy-pasted line beneath it does
    //   not (the shape the verify skill names), and AMENDING A COMMIT AN AGENT HAS ALREADY BRANCHED
    //   FROM destroys that agent's base — a follow-up commit would have cost nothing and confused
    //   nobody. Read this number off a run; never off the sentence explaining it.
    //
    // -> 44 with A12's ControlRigData. Its one field is WIRED to AnimationECSSystem::SyncControlRig, which
    // is the only place an authored rig handle becomes a pipeline stage — and before A12 there was no such
    // place at all, which is the whole of what that task was. Read off THIS branch's run; per the rule
    // above the TOTAL does not survive a merge, the DELTA does.
    //
    // -> 45 with A25's RetargetData. Its one field is WIRED to AnimationECSSystem::SyncRetarget, which is
    // the only place an authored retarget handle becomes a source rig on the Animator — and before A25
    // there was no such place at all, which is the whole of what that task was. Read off THIS branch's
    // run; per the rule above the TOTAL does not survive a merge, the DELTA does.
    EXPECT_EQ( all.size(), 45u );
}

TEST( SettingConsumers, EveryFieldNamesItsConsumer )
{
    for ( const Census& c : kCensus )
    {
        SCOPED_TRACE( c.Type );
        CheckTableCoversTypeExactly( Type( c.Type ), c.Rows, c.Count );
    }
}

// THE ASSERTION Д23 EXISTS FOR. A WIRED row used to pass when the named file merely contained the
// field's name; У3 proved that vacuous by deleting the canvas's background draw and watching this test
// stay green, because three other components in the same file have a `Sprite` too.
//
// Now the file must contain an ANCHORED READ: a member access of the field on a receiver bound, in that
// same file, to this type (setting_consumers_reader.hpp explains the shapes accepted). Deleting the read
// removes the last anchored access and this goes red; renaming the local variable moves the binding and
// the read together and it does not.
TEST( SettingConsumers, EveryNamedConsumerActuallyReadsTheFieldItClaims )
{
    using namespace Desert::Tests::ConsumerText;

    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found - run from the workspace root or build/Bin";

    for ( const Census& c : kCensus )
    {
        const std::vector<std::string> anchors = AnchorsOf( c );

        for ( const Row* r = c.Rows; r != c.Rows + c.Count; ++r )
        {
            if ( !r->Where )
                continue;

            const std::string text = StripCommentsAndLiterals( ReadFile( root + r->Where ) );
            ASSERT_FALSE( text.empty() ) << "named consumer " << r->Where << " could not be read";

            // A renderer keeps the component in a member declared in its HEADER and reads it in the
            // source, so the two files are one scope for the purpose of finding receivers. Without this
            // every `m_Data.X` in VolumetricCloudRenderer.cpp reads as unanchored, and twenty-two live
            // cloud settings look dead.
            const std::vector<std::string> receivers = DeriveReceivers(
                 text + "\n" + StripCommentsAndLiterals( ReadFile( root + SiblingOf( r->Where ) ) ), anchors );

            bool read = false;
            for ( const std::string& recv : receivers )
                read = read || ReceiverReadsField( text, recv, r->Field );
            for ( const std::string& anchor : anchors )
                read = read || AnchorReadsField( text, anchor, r->Field );

            EXPECT_TRUE( read ) << r->Where << " is named as the consumer of " << c.Type << "::" << r->Field
                                << " but contains no read of that field on a value of that type. Either "
                                   "the read was removed and the setting is now dead, or it moved to "
                                   "another file and this row must follow it.";
        }
    }
}

// THE GAP BETWEEN "SOMEBODY READ IT" AND "IT REACHED THE FRAME".
//
// Both halves of this are new in Г26 and both come from one measured defect. The procedural terrain's RockMode
// (retired in v23) was WIRED to its ECS system and the row was TRUE: the system did read the field and packed it into
// the draw command's LayerModes.y. Terrain.shader then read LayerModes.x and LayerModes.z and never .y,
// and never sampled the green splat channel — so the editor's `Rock (G)` brush, whose own overlay tells
// the user to "set the layer to 'Manual' in Details to see painted weights", painted a channel no pixel
// was computed from. This suite was green the whole time, because it asks about the FIRST link of the
// chain and the defect was in the LAST one. Г25 had fixed the identical defect one channel over
// (GrassMode / LayerModes.x) without the census noticing either.
//
// So a row may state where the value must END UP, and here that statement is checked.
namespace
{
    // The census tables are (pointer, count) pairs, which every loop in this file walks by hand. A span
    // says the same thing without the pointer arithmetic, and is what the new checks below walk.
    std::span<const Row> RowsOf( const Census& census )
    {
        return { census.Rows, census.Count };
    }

    // One row's frame anchor, checked. Split out of the TEST body because a per-row check inside two
    // nested loops is what pushes a test function past the analyser's complexity threshold, and because
    // the failure message is easier to read next to the thing it describes.
    void ExpectFrameAnchorIsRead( const std::string& root, const Census& census, const Row& row )
    {
        using namespace Desert::Tests::ConsumerText;

        ASSERT_NE( row.FrameRead, nullptr )
             << census.Type << "::" << row.Field << " names a Frame file and no expression to find in it";

        // Comments and string literals are stripped for the reason this file strips them everywhere
        // else, and here it is load-bearing rather than tidy: the terrain shader NAMES `LayerModes.y` in
        // the comment explaining why it went unread for two releases, so a plain substring search over
        // the raw text would pass on the prose that documents the defect.
        const std::string text = StripCommentsAndLiterals( ReadFile( root + row.Frame ) );
        ASSERT_FALSE( text.empty() ) << "frame anchor " << row.Frame << " could not be read";

        EXPECT_NE( text.find( row.FrameRead ), std::string::npos )
             << census.Type << "::" << row.Field << " is packed and delivered, and " << row.Frame
             << " does not read `" << row.FrameRead
             << "`. The field has a consumer and still moves nothing on screen: that is the half-way "
                "satisfaction this anchor exists to refuse.";
    }

    const Row* FindRow( const Census& census, std::string_view field )
    {
        for ( const Row& row : RowsOf( census ) )
        {
            if ( field == row.Field )
            {
                return &row;
            }
        }
        return nullptr;
    }

    // Out of the TEST body for the same reason ExpectFrameAnchorIsRead is.
    void ExpectLayerModeStatesItsFrame( const Census& census, const FieldInfo& field )
    {
        const Row* row = FindRow( census, field.Name );
        ASSERT_NE( row, nullptr ) << census.Type << "::" << field.Name << " has no census row at all";
        EXPECT_NE( row->Frame, nullptr )
             << census.Type << "::" << field.Name
             << " is a splat-layer mode and states no Frame anchor. A layer mode is honoured by a shader "
                "or by nothing: name the shader and the slot it must read, or the combo box in Details is "
                "decoration.";
    }
} // namespace

TEST( SettingConsumers, EveryFrameAnchorIsActuallyReadWhereItSaysItIs )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found - run from the workspace root or build/Bin";

    int checked = 0;
    for ( const Census& census : kCensus )
    {
        for ( const Row& row : RowsOf( census ) )
        {
            if ( row.Frame == nullptr )
            {
                continue;
            }
            ExpectFrameAnchorIsRead( root, census, row );
            ++checked;
        }
    }

    EXPECT_GT( checked, 0 ) << "no row states a frame anchor any more, so this test proves nothing";
}

// WHERE A FRAME ANCHOR IS COMPULSORY, DERIVED RATHER THAN LISTED.
//
// An opt-in anchor catches the rows somebody remembered; the next dead knob is the one nobody did. The
// smallest rule that cannot be forgotten is stated over a TYPE rather than over a field name: every
// reflected field whose type is `LandscapeLayerMode` is, by construction, an authored choice that only a
// shader can honour, so every one of them must say which slot honours it. A fourth terrain layer added
// tomorrow inherits the requirement without anybody reading this file.
//
// This is deliberately NOT stated for every field that happens to reach a shader. Most do so through a
// packing this census cannot see, and a rule that demanded an anchor it cannot derive would be answered
// with whatever token makes it pass. The enum is the case where the demand is exact.
TEST( SettingConsumers, EveryLayerModeFieldStatesWhereItReachesTheFrame )
{
    int found = 0;
    for ( const Census& census : kCensus )
    {
        for ( const FieldInfo& field : Type( census.Type ).Fields )
        {
            if ( std::string( field.TypeName ) != "LandscapeLayerMode" )
            {
                continue;
            }
            ++found;
            ExpectLayerModeStatesItsFrame( census, field );
        }
    }

    EXPECT_EQ( found, 3 ) << "the landscape's layer modes are Grass, Rock and Snow; a fourth (or a missing "
                             "one) changes what Terrain.shader has to blend and is a reviewable edit";
}

// The debt register, pinned in both directions.
//
// It is stated as an exact set rather than a count so that repairing one dead setting and introducing
// another cannot cancel out - which a count would have allowed, and which is precisely how a register
// like this stops being read.
TEST( SettingConsumers, TheKnownDeadSettingsAreExactlyThese )
{
    std::vector<std::string> fromTables;
    for ( const Census& c : kCensus )
        for ( const Row* r = c.Rows; r != c.Rows + c.Count; ++r )
            if ( r->Dead )
                fromTables.push_back( std::string( c.Type ) + "::" + r->Field );

    std::vector<std::string> registered;
    for ( const DeadSetting& d : kKnownDeadSettings )
        registered.push_back( std::string( d.Type ) + "::" + d.Field );

    std::sort( fromTables.begin(), fromTables.end() );
    std::sort( registered.begin(), registered.end() );

    EXPECT_EQ( fromTables, registered )
         << "a row marked DEAD is not in the known-debt register, or the register names a setting that is "
            "no longer dead. Both are edits somebody has to see.";

    // Zero, since Д26 retired the last three. This number going UP is a new dead setting - a §1.3
    // violation somebody has to own by name. It may not happen silently.
    EXPECT_EQ( registered.size(), 0u );
}

// ----------------------------------------------------------------------------------------------------
// THE RELATION THIS CENSUS RESTS ON, AND WHICH NOTHING ASSERTED UNTIL Д33
// ----------------------------------------------------------------------------------------------------
//
// Every row above is an argument about a source file, and every one of them is made through
// `StripCommentsAndLiterals`. That makes the reader a load-bearing part of the gate and not a utility:
// if it loses a stretch of a file, the census says "nobody reads this setting" in exactly the confident
// voice it uses when that is true. It did lose stretches. `c.Peek() == '"'` — ordinary C++, and present
// in DShaderParser.cpp — opened a string literal for a reader that knew nothing of character literals,
// and it closed at the next quote hundreds of lines away. Measured on the tree Д33 started from: the
// reader saw 11 801 tokens of that file and there are 24 793. Raw strings were the same hole in the
// other direction, their CONTENTS read as code.
//
// The relation is: A DECLARATION STANDING AFTER A CONSTRUCT IS STILL VISIBLE, whatever the construct is.
// Tested as a table over the literal forms rather than as a test per bug, because "the reader survives
// `'\"'`" is one instance of a class and the class is what has to hold — the same reason §4 of the
// verification skill prefers a relation to a function.
//
// The negative direction is asserted beside it: a read written INSIDE one of these constructs is not a
// read. Both halves are needed. A reader that deleted the whole file would pass the second alone, and a
// reader that stripped nothing would pass the first.

namespace
{
    using Desert::Tests::ConsumerText::DeriveReceivers;
    using Desert::Tests::ConsumerText::ReceiverReadsField;
    using Desert::Tests::ConsumerText::StripCommentsAndLiterals;

    // A file that binds a receiver to UICanvasData and then reads `Sprite` on it, with `before` standing
    // between the two. Whatever `before` is, the read after it must survive.
    std::string SnippetAround( const std::string& before )
    {
        return "void Draw( const ECS::UICanvasData& canvas )\n"
               "{\n"
               "    " +
               before +
               "\n"
               "    Submit( canvas.Sprite );\n"
               "}\n";
    }

    bool SpriteIsRead( const std::string& source )
    {
        const std::string              text      = StripCommentsAndLiterals( source );
        const std::vector<std::string> receivers = DeriveReceivers( text, { "UICanvasData" } );
        for ( const std::string& recv : receivers )
            if ( ReceiverReadsField( text, recv, "Sprite" ) )
                return true;
        return false;
    }

    struct LiteralForm
    {
        const char* Name;
        const char* Text;
    };

    // Each entry is a construct that has to be stepped over. The quotes and apostrophes inside them are
    // the point, so they are written with explicit escapes rather than raw strings — a raw string in the
    // TEST would be stripped by the compiler before the reader ever saw the shape being tested.
    constexpr LiteralForm kLiteralForms[] = {
         { "nothing at all (the control)", "int untouched = 0;" },
         { "a line comment", "// canvas.Sprite is named here in prose" },
         { "a line comment holding an apostrophe", "// don't let this open a character literal" },
         { "a block comment over several lines", "/* one\n     two \" three ' four\n     five */" },
         { "an ordinary string", "Log( \"a message\" );" },
         { "a string holding an escaped quote", "Log( \"say \\\" and stop\" );" },
         { "a string holding comment openers", "Log( \"http://host /* not a comment\" );" },
         { "a string holding an apostrophe", "Log( \"it's fine\" );" },
         { "a string continued by a line splice", "Log( \"first \\\n     second\" );" },
         { "a character literal", "if ( c == 'x' ) return;" },
         { "A CHARACTER LITERAL HOLDING A QUOTE", "if ( c == '\"' ) return;" },
         { "a character literal holding an escaped apostrophe", "if ( c == '\\'' ) return;" },
         { "a character literal holding a backslash", "if ( c == '\\\\' ) return;" },
         { "a character literal holding a newline escape", "if ( c == '\\n' ) return;" },
         { "a wide character literal holding a quote", "if ( w == L'\"' ) return;" },
         { "a utf-8 character literal holding a quote", "if ( w == u8'\"' ) return;" },
         { "a wide string holding a quote", "LogW( L\"say \\\" and stop\" );" },
         { "a digit separator, which is not a literal at all", "const int million = 1'000'000;" },
         { "a hexadecimal digit separator", "const unsigned mask = 0x1F'FF'00u;" },
         { "a raw string", "Log( R\"(plain)\" );" },
         { "a raw string holding quotes", "Log( R\"(he said \"stop\")\" );" },
         { "a raw string holding an apostrophe and a comment opener", "Log( R\"(it's // here)\" );" },
         { "a raw string with a delimiter", "Log( R\"json({ \"k\": 1 })json\" );" },
         { "a raw string whose body contains its own closing shape", "Log( R\"tag(a )\" inside)tag\" );" },
         { "a raw string spanning lines", "Log( R\"(one\n     two \" three)\" );" },
    };
} // namespace

// The positive half: the read written AFTER each construct is found.
TEST( SettingConsumers, TheReaderSeesCodeAfterEveryLiteralForm )
{
    for ( const LiteralForm& form : kLiteralForms )
    {
        SCOPED_TRACE( form.Name );
        EXPECT_TRUE( SpriteIsRead( SnippetAround( form.Text ) ) )
             << "a read standing after " << form.Name
             << " is invisible to the reader. Everything past this construct is missing from every "
                "census built on it, and each of them reports the settings it hides as having no "
                "consumer - a wrong answer that looks exactly like a right one.";
    }
}

// The negative half: the same read written INSIDE each construct is not a read. Only the forms that can
// carry arbitrary text are listed - a character literal and a digit separator cannot hold one.
TEST( SettingConsumers, TheReaderDoesNotSeeCodeInsideALiteralOrAComment )
{
    const std::string read = "Submit( canvas.Sprite );";
    const LiteralForm hiding[]{
         { "a line comment", "// " },
         { "a block comment", "/* " },
         { "an ordinary string", "Log( \"" },
         { "a raw string", "Log( R\"(" },
         { "a raw string with a delimiter", "Log( R\"json(" },
    };
    const char* closing[]{ "", " */", "\" );", ")\" );", ")json\" );" };

    int index = 0;
    for ( const LiteralForm& form : hiding )
    {
        SCOPED_TRACE( form.Name );
        const std::string source = "void Draw( const ECS::UICanvasData& canvas )\n{\n    " +
                                   std::string( form.Text ) + read + closing[index++] + "\n}\n";
        EXPECT_FALSE( SpriteIsRead( source ) )
             << "a read written inside " << form.Name
             << " counts as a read, so prose and log text can certify a setting nobody consumes";
    }

    // The one construct that hides the NEXT line rather than its own: a line comment ending in a
    // backslash is spliced onto the following line before comments are looked for at all, so the read
    // below it is commented out and must not count. It belongs with the negatives, not the positives —
    // and it is the only place where seeing MORE than the compiler would be the failure.
    EXPECT_FALSE( SpriteIsRead( SnippetAround( "// this comment continues \\" ) ) )
         << "a line comment ending in a backslash did not swallow the line under it, so the reader sees "
            "a read the compiler never compiles and the census certifies a dead setting";
}

// The two structural invariants, asserted directly rather than through a census.
//
// The first is what stops the whole class: an ordinary string and a character literal END AT THEIR OWN
// LINE, so a malformed one costs a line and not a file. That is the difference between the fix and a
// third workaround - `'\"'` is fixed by knowing about character literals, but the NEXT unbalanced quote
// (a `#error don't`, a stray apostrophe in a macro) would open the same hundreds-of-lines hole.
//
// The second is what let the two whole-tree censuses drop their private copies of this function: offsets
// into the output still name lines of the input, so they can keep reporting a line number.
TEST( SettingConsumers, NoLiteralConsumesMoreThanItIsAllowedTo )
{
    const std::string strayQuote = "int before = 1;\n"
                                   "const char* broken = \"unterminated;\n"
                                   "int after = 2;\n";
    const std::string stripped   = StripCommentsAndLiterals( strayQuote );
    EXPECT_NE( stripped.find( "int after" ), std::string::npos )
         << "an unterminated string swallowed the following line; the reader can still lose a whole file "
            "to one unbalanced quote";
    EXPECT_NE( stripped.find( "int before" ), std::string::npos );

    const std::string strayApostrophe = "int before = 1;\n"
                                        "int x = a; /* don't */\n"
                                        "int after = 2;\n";
    EXPECT_NE( StripCommentsAndLiterals( strayApostrophe ).find( "int after" ), std::string::npos )
         << "an apostrophe inside a comment opened a character literal";

    for ( const LiteralForm& form : kLiteralForms )
    {
        SCOPED_TRACE( form.Name );
        const std::string source = SnippetAround( form.Text );
        const std::string out    = StripCommentsAndLiterals( source );

        EXPECT_EQ( out.size(), source.size() ) << "the output is no longer byte-aligned with the input";
        EXPECT_EQ( std::count( out.begin(), out.end(), '\n' ), std::count( source.begin(), source.end(), '\n' ) )
             << "a line was lost, so an offset in the output no longer names a line of the file";
    }
}

// A field name shared by four components makes a name-only row vacuous, and this is the case that proved
// it. The row shape above is the general fix; this stays as the specific one, because it asserts
// something the general shape deliberately does not: that the handle is not merely READ but RESOLVED TO
// AN IMAGE. A canvas that tests its background handle and never draws it renders exactly like a canvas
// with no background, which is the state this setting was actually in.
//
// IT DOES NOT PIN THE LOCAL'S NAME. It used to spell `HandleSet( canvasData.Sprite )` verbatim, which
// made an honest rename of a local variable red - a failure that says nothing about whether the setting
// is alive. The receiver is now derived from the file exactly as the census's rows are.
TEST( SettingConsumers, TheCanvasBackgroundIsResolvedToAnImageAndNotJustNamedSomewhereInTheFile )
{
    using namespace Desert::Tests::ConsumerText;

    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::string source = StripCommentsAndLiterals( ReadFile( root + kCanvasRenderer ) );
    ASSERT_FALSE( source.empty() ) << kCanvasRenderer << " is missing or empty";

    const std::vector<std::string> canvas = DeriveReceivers( source, { "UICanvasData", "UICanvasComponent" } );
    ASSERT_FALSE( canvas.empty() ) << kCanvasRenderer << " no longer binds a UICanvasData at all";

    EXPECT_TRUE( CallOnFieldRead( source, "HandleSet", canvas, "Sprite" ) )
         << "UICanvasRenderer2D does not test the CANVAS's own background handle. The three other Sprite "
            "fields in this file (button, panel, image) keep the name alive whether or not the canvas's is "
            "read, which is how this setting stayed dead through a census that listed it as wired.";

    // And it is drawn, not merely inspected: the resolved image reaches the draw list.
    EXPECT_TRUE( CallOnFieldRead( source, "ResolveSpriteImage", canvas, "Sprite" ) )
         << "the canvas background handle is tested but never resolved to an image";
}

// Slot A shipped WHOLE - format, loader, service, component, collector, packer, seam and cutout in one
// task - so it owes nothing either. The pin is what keeps that true.
TEST( SettingConsumers, TheHeroCloudComponentOwesNothing )
{
    const std::ptrdiff_t pending = std::count_if( std::begin( kHeroCloudRows ), std::end( kHeroCloudRows ),
                                                  []( const Row& r ) { return r.Task != nullptr; } );

    EXPECT_EQ( pending, 0 );
}

// The fog shipped WHOLE - component, pass and couplings in one task (Sky plan Phase 5) - so it owes
// nothing. This pin is what keeps that true: a field added without its reader turns this zero into a
// reviewable edit.
TEST( SettingConsumers, TheFogComponentOwesNothing )
{
    const std::ptrdiff_t pending = std::count_if( std::begin( kFogRows ), std::end( kFogRows ),
                                                  []( const Row& r ) { return r.Task != nullptr; } );

    EXPECT_EQ( pending, 0 );
}

// The sky component now owes NOTHING. The artistic gradient was always finished; the physical
// atmosphere was built in phases (Docs/Sky/UE_SKYATMOSPHERE_RESEARCH.md section 4) and Phase 3 — the
// camera aerial-perspective volume and its apply on opaque — consumed the last two fields that were
// carried without a reader, plus the Aerial Perspective Distance it added.
//
// The count stays as a count rather than becoming "no PENDING rows exist", because a later phase may
// well add a field before it adds its reader. When that happens the number rises in a reviewable edit
// instead of a field quietly joining the component with nobody accountable for it.
TEST( SettingConsumers, TheSkyComponentOwesNothing )
{
    const std::ptrdiff_t pending = std::count_if( std::begin( kSkyRows ), std::end( kSkyRows ),
                                                  []( const Row& r ) { return r.Task != nullptr; } );

    EXPECT_EQ( pending, 0 );
}

// The cloud component shipped the same way the fog did - component, packer, bake and march in one
// programme - so it owes nothing either. The count is a count rather than "no PENDING rows exist" for the
// reason the two above give: a later phase may well add a field before it adds its reader, and when that
// happens the number has to rise in a reviewable edit instead of a field quietly joining the component
// with nobody accountable for it.
//
// It matters more here than anywhere else in this file. A cloud parameter that does nothing still LOOKS
// like it does, because the sky it is supposed to change is already busy - which is precisely why this
// programme's contract forbids a knob that moves nothing.
TEST( SettingConsumers, TheCloudComponentOwesNothing )
{
    const std::ptrdiff_t pending = std::count_if( std::begin( kCloudRows ), std::end( kCloudRows ),
                                                  []( const Row& r ) { return r.Task != nullptr; } );

    EXPECT_EQ( pending, 0 );
}

// A slider can name its consumer and STILL not reach it, and this is the case that proves it.
//
// Light March Samples travels to the GPU through three separate ceilings: the Range on the PROPERTY, the
// std::clamp in Graphic::PackCloudParams, and a clamp written again inside the compute shader. All three
// were the literal 16. Raise the first two to 64 and forget the third and the artist drags the slider to
// 64, the payload carries 64, and the march silently uses 16 — a setting that reaches its consumer and is
// thrown away there, which no reflection test and no build can see.
//
// The first two ceilings are now one constant. The shader cannot include a C++ header, so its copy is
// checked the only way it can be: by reading the shader's own text. That is what makes this an assertion
// about the RELATION (contract 2.3.1) rather than three assertions about the number 64.
TEST( SettingConsumers, TheShaderClampsTheShadowRayAtTheSameCeilingTheSliderOffers )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::string path   = root + "Editor/Resources/Shaders/Programs/Clouds/CloudRaymarch.shader";
    const std::string source = ReadFile( path );
    ASSERT_FALSE( source.empty() ) << path << " is missing or empty";

    // The exact line the march clamps on. Written out in full rather than matched loosely, because a
    // regex that still matched after somebody rewrote the clamp would pass while testing nothing.
    const std::string expected = "int   lightSamples = int(clamp(u_CloudSunColour.w, 1.0f, " +
                                 std::to_string( Desert::ECS::kCloudLightMarchMaxSamples ) + ".0f));";

    EXPECT_NE( source.find( expected ), std::string::npos )
         << "CloudRaymarch.shader does not clamp the shadow ray's sample count at "
         << Desert::ECS::kCloudLightMarchMaxSamples << ", which is the ceiling the slider offers and the "
         << "payload packs. Expected to find:\n  " << expected;
}

// The same three-ceilings shape as the test above, for the multiple-scattering octave count.
//
// It was three literal 3s — the PROPERTY's Range, the std::clamp in Graphic::PackCloudParams and a clamp
// written again in the march — and they agreed only because nobody had moved one. Р18 had reason to move
// it (the question was whether more octaves recover what the approximation loses at a physical
// extinction; measured, they make it worse, which is recorded on the constant itself), and the first
// thing that question needs is for the three to be one.
//
// THE CLAMP IS IN THE HEADER, NOT THE SHADER, and the difference is the point of the change rather than
// an accident of it: the series moved into Common/CloudLighting.glslh so a test could drive it as C++.
// This assertion therefore reads a .glslh where its neighbour above reads a .shader.
TEST( SettingConsumers, TheScatteringSeriesClampsItsOctavesAtTheSameCeilingTheSliderOffers )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::string path   = root + "Editor/Resources/Shaders/Common/CloudLighting.glslh";
    const std::string source = ReadFile( path );
    ASSERT_FALSE( source.empty() ) << path << " is missing or empty";

    const std::string expected =
         "#define CLOUD_MAX_SCATTER_OCTAVES " + std::to_string( Desert::ECS::kCloudMultiScatterMaxOctaves );

    EXPECT_NE( source.find( expected ), std::string::npos )
         << "CloudLighting.glslh does not cap the multiple-scattering series at "
         << Desert::ECS::kCloudMultiScatterMaxOctaves << ", which is the ceiling the slider offers and the "
         << "payload packs. Expected to find:\n  " << expected;
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

#pragma once

#include <CoreReflection/ReflectionMacros.hpp>

#include <cstdint>

// LIGHTMASS CONTRACT (owner backlog LIGHTMASS; lead decisions Q1-Q8 of 10-22) — the ECS half of baked static
// lighting. HEADER CONTRACT ONLY: nothing includes this yet.
//
// Q1: THERE IS NO STATIC-LIGHTING COMPONENT. These are settings BLOCKS, each embedded as a member of a component
// that already exists, exactly as UE keeps LightmassSettings on the light / primitive / world (docs/contracts/
// LIGHTMASS.md, "Scene format"):
//   StaticLightingPrimitiveSettings  -> StaticMeshComponent, InstancedStaticMeshComponent, LandscapeComponent
//                                       (member `StaticLighting`)
//   StaticLightingLightmapSettings   -> StaticMeshComponent, InstancedStaticMeshComponent (not a foliage field),
//                                       LandscapeComponent (member `Lightmap`)
//   StaticLightingLightSettings      -> DirectionalLightData, PointLightData, SpotLightData (member
//   `StaticLighting`)
// The level's bake settings are Graphic::StaticLighting::LightmassSettings in the World Settings
// (LightmassBake.hpp).
//
// UE counterparts: EComponentMobility (USceneComponent::Mobility), UPrimitiveComponent::bOverrideLightMapRes /
// OverriddenLightMapRes / bCastStaticShadow / LightmassSettings, ULightComponent::IndirectLightingIntensity /
// LightmassSettings, ULandscapeComponent / ALandscapeProxy::StaticLightingResolution.
//
// Q7, WHAT IS LIGHTMAPPED: static meshes, non-foliage instanced static meshes (one lightmap per instance) and
// the landscape (one lightmap per LandscapeTileComponent entity, the resolution from the root's block). A
// FOLIAGE FIELD (InstancedStaticMeshComponent + FoliageComponent) and the landscape's GRASS are NOT lightmapped:
// they read the volumetric lightmap (UE gives foliage per-instance lightmaps; the lead chose the VLM for
// thousands of instances). A foliage field still carries StaticLightingPrimitiveSettings (Static foliage casts
// into the bake); the Details panel does not draw its `Lightmap` block and the bake does not read it. Grass is
// generated per frame, is not in the static set and casts no baked shadow.
//
// WHERE THIS ENGINE DIFFERS: UE puts Mobility on every USceneComponent; here it is stated only where it changes
// what is rendered — on the lit primitives and on the three analytic lights.
namespace Desert::ECS
{
    // UE EComponentMobility. The ORDER is the ladder from "everything baked" to "nothing baked"; stored by number.
    enum class Mobility : uint8_t
    {
        // Primitive: lit by its lightmap (or the VLM for a foliage field) — indirect, the direct light of Static
        // lights and the stationary shadow factors; never moves at runtime: a transform write on a Static
        // primitive in play is refused with an error (UE warns; here the lightmap would silently lie).
        // Light: contributes ONLY through the bake (direct, indirect, shadows); no runtime cost. Q2: no runtime
        // shadow pass draws it — no VSM page, no cascade, no traced ray.
        Static = 0,
        // Light only (a primitive cannot be Stationary — UE allows it and treats it as Static for lighting):
        // direct light, colour and intensity are evaluated every frame; Q2: its shadow is
        //     min( baked static-geometry shadow, the runtime shadow method over MOVABLE casters only ),
        // the baked part a distance-field shadow map channel (at most 4 overlapping stationary lights per texel,
        // UE's limit). Q4: its indirect is baked at the intensity and colour of the bake and is NOT rescaled at
        // runtime (UE); an edit of Intensity or Color stales the build ("Lighting needs to be rebuilt").
        Stationary,
        // Neither baked nor read from the bake: lit and shadowed by the resolved methods every frame. A Movable
        // primitive in a lightmapped view reads its indirect from the volumetric lightmap.
        Movable,
    };

    // On StaticMeshComponent / InstancedStaticMeshComponent / LandscapeComponent (member `StaticLighting`).
    struct StaticLightingPrimitiveSettings
    {
        REFLECT()

        PROPERTY(
             DisplayName( "Mobility" ), Category( "Static Lighting" ), Summary,
             Tooltip( "Static: lit by its lightmap (a foliage field: the volumetric lightmap) and never moved "
                      "at runtime. Movable: lit every frame by the view's methods, its indirect from the "
                      "volumetric lightmap when the view's GI is Lightmaps. Stationary is a light-only "
                      "mobility." ) )
        Mobility PrimitiveMobility = Mobility::Movable;

        PROPERTY( DisplayName( "Cast Static Shadow" ), Category( "Static Lighting" ),
                  Tooltip( "Whether this primitive blocks light in the bake (UE: bCastStaticShadow). Separate "
                           "from Cast Shadows, which is the runtime shadow passes." ) )
        bool CastStaticShadow = true;

        PROPERTY( DisplayName( "Diffuse Boost" ), Category( "Static Lighting" ), Advanced, Range( 0.0f, 10.0f ),
                  Units( "x" ),
                  Tooltip( "Multiplier on the diffuse albedo this primitive reflects in the bake's bounces (UE: "
                           "FLightmassPrimitiveSettings::DiffuseBoost)." ) )
        float DiffuseBoost = 1.0f;
    };

    // On StaticMeshComponent / InstancedStaticMeshComponent / LandscapeComponent (member `Lightmap`): only on what
    // owns a lightmap (Q7). Q6: the resolution is LOD 0's; every LOD gets its OWN lightmap region, each LOD's side
    // scaled by the LOD's screen-size ratio to LOD 0 and rounded up to a multiple of 4 (UE bakes each LOD).
    struct StaticLightingLightmapSettings
    {
        REFLECT()

        PROPERTY( DisplayName( "Override Lightmap Resolution" ), Category( "Static Lighting" ),
                  Tooltip( "Off: the mesh asset's Lightmap Resolution (UE: UStaticMesh::LightMapResolution; the "
                           "landscape: 4 texels per quad). On: this entity's own (UE: bOverrideLightMapRes)." ) )
        bool OverrideLightmapResolution = false;

        PROPERTY( DisplayName( "Lightmap Resolution" ), Category( "Static Lighting" ), Range( 4, 4096 ),
                  Units( "texels" ),
                  Tooltip( "Side of LOD 0's square lightmap region in texels (a landscape: per tile), rounded up "
                           "to a multiple of 4 (the block size of the compressed lightmap). UE's default, 64." ) )
        int32_t LightmapResolution = 64;
    };

    // On DirectionalLightData / PointLightData / SpotLightData (member `StaticLighting`).
    struct StaticLightingLightSettings
    {
        REFLECT()

        PROPERTY( DisplayName( "Mobility" ), Category( "Static Lighting" ), Summary,
                  Tooltip( "Static: baked only (direct, indirect and shadows), no runtime cost. Stationary: "
                           "dynamic direct light, shadowed by the baked static geometry and the runtime shadows "
                           "of movable objects; baked indirect. Movable: nothing baked. Changing Intensity or "
                           "Color of a Static or Stationary light requires rebuilding lighting." ) )
        Mobility LightMobility = Mobility::Movable;

        PROPERTY( DisplayName( "Indirect Lighting Intensity" ), Category( "Static Lighting" ),
                  Range( 0.0f, 10.0f ), Units( "x" ),
                  Tooltip( "Scale on this light's baked bounce (UE: ULightComponent::IndirectLightingIntensity). "
                           "The runtime GI methods ignore it, as UE's Lumen honours its own." ) )
        float IndirectLightingIntensity = 1.0f;

        PROPERTY( DisplayName( "Bake Shadow Exponent" ), Category( "Static Lighting" ), Advanced,
                  Range( 0.5f, 8.0f ),
                  Tooltip( "Penumbra falloff of the baked area shadow (UE: FLightmassLightSettings::"
                           "ShadowExponent). The penumbra's width is the light's own Source Radius / Angle." ) )
        float BakeShadowExponent = 2.0f;

        // NOT HERE, unlike UE's ULightComponent::ShadowMapChannel: the stationary light's shadow channel is a
        // RESULT of the bake and lives only in the build data (Assets::BakedLightRecord::ShadowMapChannel), so a
        // scene file never carries a channel that disagrees with the build it was baked into.
    };
} // namespace Desert::ECS

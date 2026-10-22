#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// LIGHTMASS CONTRACT (owner backlog LIGHTMASS; lead decisions Q4, Q6, Q7, Q8 of 10-22) — WHAT A BAKE DEPENDS ON.
// Header contract only; no definitions yet.
//
// ONE SOURCE OF TRUTH FOR "IS THE LIGHTING CURRENT": the bake, the editor's "Lighting needs to be rebuilt" overlay
// and the cook all gather the static set through GatherStaticLightingInputs, over the SAME serialized scene bytes
// (the editor serializes its live scene in memory, as it does for a save; the cook reads the `.desce`). So the
// hash a record was baked with and the hash the cook checks are computed by one function from one form.
//
// Q8: THE BAKE IS AN EDITOR STEP (Build > Build Lighting), never a cook step — as the HLODs are packed, not built
// (Core::WorldCells::CookWorld, WP-FAR-6B D1). The cook PACKAGES the saved LightingBuildDataAsset and is REFUSED,
// naming every stale or unbaked entity, when CheckLightingBuildCurrent fails. No headless bake, no RT GPU on the
// cook machine.
namespace Desert::Assets
{
    class LightingBuildDataAsset;

    enum class StaticLightingInputKind : uint8_t
    {
        Primitive = 0, // a Static static mesh, one non-foliage ISM instance, or one landscape tile (lightmapped)
        VolumeOnly,    // a Static foliage instance: casts into the bake, lit by the volumetric lightmap (Q7)
        Light,         // a Static or Stationary light
    };

    // One entry of the static set.
    struct StaticLightingInput
    {
        StaticLightingInputKind Kind = StaticLightingInputKind::Primitive;
        Common::UUID            Entity;
        uint32_t InstanceIndex = 0; // the ISM instance; 0 for a static mesh, a landscape tile, a light
        uint32_t LodCount      = 1; // Q6: one lightmap per LOD (Primitive only)
        // Side of LOD 0's region after the entity override, the mesh asset's default and the level's
        // StaticLightingLevelScale (Primitive only).
        uint32_t LightmapResolution = 0;
        // Hash of every input that shapes this entry's bake: for a primitive its transform, the mesh asset's
        // content hash (or the tile's height and weight data), its two settings blocks; for a light its type,
        // mobility, transform, Intensity, Color (Q4: a changed intensity or colour stales the build, the bounce
        // is not rescaled at runtime), attenuation, cone, source size and StaticLightingLightSettings. The
        // primitive's hash also folds in the hashes of the lights reaching it.
        uint64_t    InputHash = 0;
        std::string EntityName; // what the overlay and the cook's refusal print
    };

    struct StaticLightingInputs
    {
        uint64_t                         SettingsHash = 0; // the World Settings' LightmassSettings
        std::vector<StaticLightingInput> Entries;
    };

    // The static set of @p scene: every Static primitive (one entry per ISM instance and per landscape tile) and
    // every Static / Stationary light. Refused when a referenced mesh asset is missing from @p registries.
    [[nodiscard]] Common::ResultStr<StaticLightingInputs>
    GatherStaticLightingInputs( const Core::SceneSerialized&                  scene,
                                std::span<const Common::Utils::AssetRegistry> registries );

    // The entries of @p inputs that @p build does not hold, or holds with another InputHash, plus all of them when
    // the settings hash differs. Empty: the build is current.
    [[nodiscard]] std::vector<const StaticLightingInput*>
    StaleLightingEntries( const LightingBuildDataAsset& build, const StaticLightingInputs& inputs );

    // The cook's gate (Q8): true when the build is current; otherwise an error "lighting is stale: rebuild
    // lighting in the editor" followed by the names of the stale entries. A scene with an empty static set needs
    // no build (true with a null @p build); a non-empty one with a null @p build is refused the same way.
    [[nodiscard]] Common::BoolResultStr CheckLightingBuildCurrent( const LightingBuildDataAsset* build,
                                                                   const StaticLightingInputs&   inputs );
} // namespace Desert::Assets

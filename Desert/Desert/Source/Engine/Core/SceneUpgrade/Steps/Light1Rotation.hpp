#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    // THE LIGHT COMPONENTS' OWN CHAIN (Assets::kLightSchemaTag), run after the asset-reference chain.
    //   1 - A DIRECTIONAL LIGHT IS AIMED BY ITS ROTATION (PORT-SUN-ROTATION), as in UE: the light travels along
    //       the entity's world forward axis (-Z) and its Translation is only where it sits. Every record with a
    //       DirectionLight block whose Translation is a direction (longer than 1e-4, the old engine's gate) gets
    //       the Rotation that aims its world forward axis along normalize(Translation) - the parents' stated
    //       Rotations divided out - and a zero Translation. A light whose Translation was no direction emitted
    //       nothing before; it keeps its Rotation and is counted. A prefab override that states a Translation
    //       beside a DirectionLight block is converted the same way, its parent rotation taken as identity, and
    //       counted (MigrateLightRotationV0ToV1). Scenes and prefabs alike.
    inline constexpr int kLightVersionRotation = 1;

    // What MigrateLightRotationV0ToV1 did to one file.
    struct SunRotationReport
    {
        std::size_t Aimed      = 0; // records whose Translation became their Rotation
        std::size_t Overrides  = 0; // prefab overrides converted with an identity parent rotation
        std::size_t Undirected = 0; // lights whose Translation was no direction; Rotation kept as stated
    };

    // PURE (no filesystem, no globals): edits the records in place.
    SunRotationReport MigrateLightRotationV0ToV1( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunLightRotation( UpgradeContext& context );
} // namespace Desert::Core

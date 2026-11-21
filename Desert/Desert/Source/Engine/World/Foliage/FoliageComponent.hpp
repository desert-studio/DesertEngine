#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Assets/Common.hpp>

namespace Desert::ECS
{
    // A FOLIAGE FIELD (UE: one FFoliageInfo of an AInstancedFoliageActor). Sits beside an
    // InstancedStaticMeshComponent that holds the painted instances; WHAT is painted — the mesh and the scatter
    // numbers — is the `.defoliage` named here (Assets::FoliageTypeAsset), shared by every field painted with
    // the same type. Saved as {FoliageTypeGuid, FoliageTypePath}; a type the project does not have refuses the
    // scene's load with both (ComponentRegistry.cpp) instead of painting with defaults.
    struct FoliageComponent
    {
        Assets::AssetHandle FoliageType;

        // RUNTIME, never saved: what a Prefab-type field's children were last realized from
        // (World::Foliage::RealizePrefabFoliage). A field whose instances, child count, world transform, type and
        // prefab all match it is skipped - realized on an edit, not re-reconciled every frame (UE: foliage
        // instances change only by an edit).
        struct PrefabRealization
        {
            uint64_t            InstanceRevision = 0; // InstancedStaticMeshComponent::GetInstanceRevision
            size_t              ChildCount       = 0;
            glm::mat4           FieldWorld{ 0.0f };
            Assets::AssetHandle Type;
            const void*         Prefab = nullptr; // the PrefabAsset realized; null = never realized
        };
        PrefabRealization RuntimePrefabRealized;
    };
} // namespace Desert::ECS

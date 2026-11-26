#pragma once

#include <Common/Core/Entt.hpp>

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
#include <Engine/Assets/Prefab/PrefabRuntimeBehavior.hpp>

namespace Desert::ECS
{
    struct PrefabComponent
    {
        Assets::AssetHandle Prefab;
        // This instance's override of how it streams in a partitioned world (WP21-WHOLE; UE ALevelInstance::
        // DesiredRuntimeBehavior); absent = what the prefab states (PrefabAsset::GetRuntimeBehavior). Saved as the
        // instance record's RuntimeBehavior; Play and the cook read it from there (ExpandPrefabInstances).
        std::optional<Assets::PrefabRuntimeBehavior> RuntimeBehavior;
    };
} // namespace Desert::ECS

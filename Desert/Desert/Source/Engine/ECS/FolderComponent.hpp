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

namespace Desert::ECS
{
    // Scene-outliner grouping node: an otherwise-empty entity that acts as a FOLDER for organizing the
    // hierarchy (drag entities under it). It renders nothing — just parents children via RelationshipComponent
    // — so it's purely an authoring aid. Marker component (no data); serialized so folders persist.
    struct FolderComponent
    {
    };
} // namespace Desert::ECS

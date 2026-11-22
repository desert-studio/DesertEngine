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

namespace Desert::ECS
{
    struct UUIDComponent
    {
        // The one place in the engine that WANTS a fresh random id per construction: every entity must be
        // distinguishable from every other, and there is no path or file to derive that from. Spelled out
        // because `Common::UUID` alone is null — see UUID.hpp.
        Common::UUID UUID = Common::UUID::Generate();
    };
} // namespace Desert::ECS

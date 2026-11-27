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
    // One overridden material parameter (keyed by the shader's Properties name). vec4 stores any
    // scalar/vector value (float uses .x, color uses rgba). Data-driven: NOT a fixed C++ field per param.
    struct MaterialParamOverride
    {
        std::string Name;
        glm::vec4   Value = glm::vec4( 0.0f );

        [[nodiscard]] bool operator==( const MaterialParamOverride& ) const = default;
    };
} // namespace Desert::ECS

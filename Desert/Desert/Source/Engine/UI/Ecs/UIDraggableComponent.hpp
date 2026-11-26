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

#include <UI/Args/UIEventArgs.hpp>

namespace Desert::ECS
{
    struct UIDraggableComponent
    {
        COMPONENT( Key( "UIDraggable" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIDraggableData Data;
    };
} // namespace Desert::ECS

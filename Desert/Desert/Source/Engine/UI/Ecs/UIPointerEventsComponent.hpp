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

#include <UI/Args/UIEventArgs.hpp>

namespace Desert::ECS
{
    struct UIPointerEventsComponent
    {
        COMPONENT( Key( "UIPointerEvents" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIPointerEventsData Data;
    };
} // namespace Desert::ECS

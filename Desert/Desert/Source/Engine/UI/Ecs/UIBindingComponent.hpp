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

#include <UI/Args/UIBindingArgs.hpp>

namespace Desert::ECS
{
    struct UIBindingComponent
    {
        COMPONENT( Key( "UIBinding" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIBindingData Data;
    };
} // namespace Desert::ECS

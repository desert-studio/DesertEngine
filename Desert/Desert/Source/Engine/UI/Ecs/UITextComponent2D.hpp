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

#include <UI/Args/UITextArgs.hpp>

namespace Desert::ECS
{
    struct UITextComponent2D
    {
        COMPONENT( Key( "UIText" ), Block( Data ), Run( ActorsAndUI ) )
        UI::UITextData Data;
    };
} // namespace Desert::ECS

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

#include <UI/Args/UIScrollArgs.hpp>

namespace Desert::ECS
{
    struct UIScrollViewComponent
    {
        COMPONENT( Key( "UIScrollView" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIScrollViewData Data;
    };
} // namespace Desert::ECS

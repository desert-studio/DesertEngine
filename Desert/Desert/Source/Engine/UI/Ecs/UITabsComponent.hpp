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

#include <UI/Args/UIScreenArgs.hpp>

namespace Desert::ECS
{
    // A tab bar: the ShowScreen buttons below it select themselves from the switcher they drive
    // (UI::UITabsData, UI/UIScreenMachine.hpp). A modifier on a container element; draws nothing itself.
    struct UITabsComponent
    {
        COMPONENT( Key( "UITabs" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UITabsData Data;
    };
} // namespace Desert::ECS

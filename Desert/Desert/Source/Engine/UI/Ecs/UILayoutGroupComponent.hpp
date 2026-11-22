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

#include <UI/Args/UILayoutArgs.hpp>

namespace Desert::ECS
{
    // ============================================================
    // UI — Godot-Control-style screen-space UI (2D). The authored data lives in the framework
    // (Desert/UI/Source/UI/Args, namespace Desert::UI); the ECS stores each one in the component that wraps it.
    // ============================================================

    struct UILayoutGroupComponent
    {
        COMPONENT( Key( "UILayoutGroup" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UILayoutGroupData Data;
    };
} // namespace Desert::ECS

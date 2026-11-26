#pragma once

#include <UI/UIWorld.hpp>

#include <Common/Core/Entt.hpp>

namespace TestSupport
{
    // A registry a UI walk accepts: it carries the UIWorld a Scene's registry carries.
    inline entt::registry UIRegistry()
    {
        entt::registry registry;
        registry.set<Desert::UI::UIWorld>();
        return registry;
    }
} // namespace TestSupport

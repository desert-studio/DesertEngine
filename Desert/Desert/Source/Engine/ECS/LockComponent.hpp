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
    // Authoring lock (UE's actor lock): this entity cannot be picked in the viewport and its transform
    // cannot be dragged by the gizmo. Set on a finished floor, a lightmap-baked prop, a background you
    // keep grabbing by accident.
    //
    // MARKER, not a `bool Locked` — presence IS the state, the same bargain FolderComponent makes above.
    // A bool would have two ways to spell "not locked" (absent, or present-and-false) and every reader
    // would have to handle both; a marker has one, so `registry.has<LockComponent>( e )` is the whole
    // question and no site can get it half-right.
    //
    // IT IS SERIALIZED (ComponentRegistry.cpp, "Lock"). That is not decoration: a lock that does not
    // survive a reload protects nothing, because the reload is exactly when you have forgotten which
    // things were finished. Adding the key needs no scene-version bump — ForeignKeys preserves keys a
    // build does not declare, and a file written before this component simply lacks it and loads
    // unlocked, which is the correct default.
    //
    // NOT a render or gameplay flag: nothing in the runtime reads it, and a packaged game has no
    // viewport to pick in. VisibilityComponent is the one that changes what is drawn.
    struct LockComponent
    {
    };
} // namespace Desert::ECS

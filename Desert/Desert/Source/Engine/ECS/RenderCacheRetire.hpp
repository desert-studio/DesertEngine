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

#include <Engine/Core/FrameManager.hpp>

namespace Desert::ECS
{
    // A COMPONENT'S RENDER CACHES ARE DROPPED THROUGH HERE, NEVER JUST RESET (UE: an RHI object the game thread
    // releases is deleted once no frame in flight names it). The draws of the frame being recorded name a cached
    // mesh and its material instances by raw pointer, and the render task records them while the interface edits
    // or removes the component (RenderTask): a reset here was the last owner and freed them under the task, and
    // under the GPU frames before it. Engine::FrameManager::RetireAfterFrames keeps them past both. The writers
    // below park what they drop; Core::Scene parks a destroyed component's caches (on_destroy).
    template <typename T>
    inline void RetireRenderCache( const std::shared_ptr<T>& object )
    {
        Engine::FrameManager::GetInstance().RetireAfterFrames( object );
    }
    template <typename T>
    inline void RetireRenderCaches( const std::vector<std::shared_ptr<T>>& objects )
    {
        for ( const auto& object : objects )
            RetireRenderCache( object );
    }
} // namespace Desert::ECS

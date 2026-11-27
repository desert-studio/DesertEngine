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
    // WORLD PARTITION: KEEP THIS LOADED EVERYWHERE. The author's one input to partitioning besides the
    // grid (owner decision 2026-09-23): the level and the cell are derived from the footprint, whether a
    // thing is global is derived from its components (Rules::kComponentLoading), and this marker is for
    // what neither can see - a game-manager script, a trigger that must hear the player from anywhere.
    //
    // MARKER for the reason LockComponent above is one: presence is the state, so there is no
    // present-and-false second spelling of "not always-loaded". It reaches the partition through its key
    // on disk, "AlwaysLoaded" (ComponentRegistry.cpp), which Rules::PlanWorldPartition reads; one marked
    // member makes its whole composite always-loaded, because a composite is never divided.
    //
    // In a world with no WorldPartition block it changes nothing, and its toggle in the Details header
    // says so in its tooltip.
    struct AlwaysLoadedComponent
    {
    };
} // namespace Desert::ECS

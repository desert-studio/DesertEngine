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
    // WHICH RECORD OF WHICH PREFAB FILE THIS ENTITY CAME FROM — the link an override is addressed by.
    //
    // RUNTIME ONLY, AND DELIBERATELY NOT SERIALIZED. PrefabFactory stamps it on every entity it creates,
    // at every nesting depth, so it is re-derived in full on every instantiation; writing it into a
    // `.desce` would be a second copy of a fact the prefab file already states, and the two would drift
    // the first time a prefab was edited.
    //
    // WHY THE ENTITY'S OWN UUID COULD NOT BE USED INSTEAD. PrefabFactory mints a FRESH uuid for every
    // entity of every instance on purpose (two instances of one prefab must not collide), so an instance
    // entity has no identity that survives a reload. The record id does: it lives in the `.deprefab` and
    // only changes when the prefab itself is re-authored.
    //
    // The vector is a PATH, not an id: one element for an entity of the instance itself, two for one
    // inside a prefab nested a level down, and so on.
    struct PrefabInstanceComponent
    {
        std::vector<Common::UUID> SourcePath;
    };
} // namespace Desert::ECS

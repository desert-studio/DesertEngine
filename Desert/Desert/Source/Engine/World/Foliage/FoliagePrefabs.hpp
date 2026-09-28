#pragma once

// PREFAB FOLIAGE (FO-8) — UE UFoliageType_Actor: every instance of the type is a real entity with its children.
//
// UE keeps the instance transforms in the FFoliageInfo and, for an actor type, an FFoliageActor beside them that
// spawns one actor per transform (FoliageActor.cpp: FFoliageActor::AddInstance / RemoveInstance / MoveInstance).
// Here the same split: the field's InstancedStaticMeshComponent::InstanceTransforms stay the one statement of
// where the instances stand — every brush mode, the undo, the World Partition filing and the OFPA save work on
// them unchanged — and the prefab instances are REALIZED from them as the field's children. A realized child is
// derived state: it is never saved (SceneSerializer skips a foliage field's descendants), it is rebuilt when the
// field loads or streams in, and it goes when the field goes (Scene::DestroyEntity takes the tree).
//
// The rule (ApplyPrefabFoliage) is written against a host so it is tested without a scene; the scene half
// (RealizePrefabFoliage) runs once per frame on the main thread before the systems read the ECS.

#include <Common/Core/ResultStr.hpp>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::World::Foliage
{
    /// What realizing a field takes: how many instances to create or destroy (at the END of the child list,
    /// so the kept ones keep their entities) and which kept ones stand somewhere else now.
    struct PrefabFoliagePlan
    {
        std::size_t              Create  = 0;
        std::size_t              Destroy = 0;
        std::vector<std::size_t> Moved;

        [[nodiscard]] bool Empty() const
        {
            return Create == 0 && Destroy == 0 && Moved.empty();
        }
    };

    /// The plan that turns @p realized (the children's local transforms, in child order) into @p wanted (the
    /// field's instance transforms). A child is moved when any matrix element differs by more than 1e-4.
    [[nodiscard]] PrefabFoliagePlan PlanPrefabFoliage( std::span<const glm::mat4> wanted,
                                                       std::span<const glm::mat4> realized );

    /// Where a realization writes. Spawn appends one prefab instance as the field's LAST child (its transform
    /// is set by Place right after); DestroyLast removes the last child with its subtree.
    struct PrefabFoliageHost
    {
        std::function<std::vector<glm::mat4>()>              Realized;
        std::function<Common::BoolResultStr()>               Spawn;
        std::function<void()>                                DestroyLast;
        std::function<void( std::size_t, const glm::mat4& )> Place;
    };

    /// Brings the host's children to @p wanted: destroys the surplus, spawns the missing, places every child
    /// whose transform differs. The first Spawn that fails stops the pass and is returned with the index.
    Common::BoolResultStr ApplyPrefabFoliage( std::span<const glm::mat4> wanted, const PrefabFoliageHost& host );

    /// The header GUID of the `.deprefab` at @p file — the identity a Prefab foliage type names it by.
    [[nodiscard]] Common::ResultStr<std::string> PrefabFileGuid( const std::filesystem::path& file );

    /// Realizes every Prefab-type field of @p scene (see the file comment). A type or prefab that cannot be
    /// read is said once by FoliageTypeService and its fields stay unrealized.
    void RealizePrefabFoliage( Core::Scene& scene );
} // namespace Desert::World::Foliage

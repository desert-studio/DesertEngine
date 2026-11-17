#pragma once

// HOW A PREFAB INSTANCE STREAMS IN A PARTITIONED WORLD (WP21-WHOLE) - UE 5.5
// ALevelInstance::DesiredRuntimeBehavior (ELevelInstanceRuntimeBehavior), the pattern and not the letter.
//
// UE gives a Level Instance two runtime behaviours: Partitioned (its actors are embedded in the outer World
// Partition and each streams by its own bounds) and LevelStreaming (the level streams whole, as one
// ULevelStreamingLevelInstance placed by the instance's bounds). Here the second is the same plan, cook and
// residency as everything else: the cook keeps the expanded body under the instance root (Core::WorldCells::
// ExpandPrefabInstances), the root -> body links are containment, and a composite is never divided - so the whole
// instance is ONE composite whose footprint is the union of its body's, it lands in one cell, the HLOD of that
// cell stands in for all of it, and moving the root at runtime carries the body. No second streaming subsystem.
//
// STATED BY THE PREFAB (Assets::PrefabData::RuntimeBehavior, the default every instance of it streams by) AND, as
// an override, BY AN INSTANCE (Assets::EntityData::RuntimeBehavior / ECS::PrefabComponent::RuntimeBehavior: absent
// = the prefab's). World Partition format v4 (Core::kWorldPartitionVersion).

#include <cstdint>
#include <optional>

namespace Desert::Assets
{
    enum class PrefabRuntimeBehavior : std::uint8_t
    {
        // UE ELevelInstanceRuntimeBehavior::Partitioned: the body's top-level records leave the instance root and
        // each is placed by its own bounds, so a prefab spanning many cells streams cell by cell. Moving the root
        // at runtime does not carry them (in a cooked world they are no longer its children).
        Partitioned,
        // UE ELevelInstanceRuntimeBehavior::LevelStreaming: the instance streams whole - one composite, one cell,
        // one HLOD source - and its body stays the root's children, so moving the root carries it.
        Standalone,
    };

    // What an instance streams by: its own override when it states one, else what its prefab states.
    [[nodiscard]] constexpr PrefabRuntimeBehavior
    ResolvePrefabRuntimeBehavior( const std::optional<PrefabRuntimeBehavior>& instance,
                                  PrefabRuntimeBehavior                       prefab )
    {
        return instance.has_value() ? *instance : prefab;
    }

    [[nodiscard]] constexpr const char* PrefabRuntimeBehaviorName( PrefabRuntimeBehavior behavior )
    {
        return behavior == PrefabRuntimeBehavior::Standalone ? "Standalone" : "Partitioned";
    }
} // namespace Desert::Assets

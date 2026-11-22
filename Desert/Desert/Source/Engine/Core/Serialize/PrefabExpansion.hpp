#pragma once

// PREFAB INSTANCES EXPANDED AT COOK (WP21) - UE 5.x embedded Level Instance.
//
// A scene record that names a prefab (EntityData::PrefabPath) is ONE placement in the source world; the cook
// must not place it as one unit, or a prefab spanning many cells lands whole in one cell and its HLOD skips it.
// This step turns every instance into the records the loader would create for it, with no ECS:
//   1. the body is stitched as PrefabFactory stitches it (Rules::PlanSceneStitch, InstantiatedLater), nested
//      prefabs recursively (a cycle is refused by path), each nesting record's overrides laid over its body;
//   2. the instance record's PrefabOverrides are laid over the body (Assets::LayerOverrideOntoRecord, the
//      applier the suite holds equal to the live one); an override that addresses no body record is REFUSED
//      by name - the loader only warns, a cook must not write a world that differs from the editor silently;
//   3. the root takes the instance record's id and its stated T/R/S (PlaceInstanceRoot order: after the
//      overrides); every other record gets a STABLE id = FNV-1a 64 of (instance id, PrefabPathKey(path)), so
//      the cook stays byte-reproducible; `parent` and every Rules::kEntityReferences field naming a body record
//      are remapped to the new ids (PrefabFactory mints fresh ids and does NOT remap them - a factory defect);
//   4. THE SPLIT, by the instance's runtime behaviour (WP21-WHOLE; Assets::PrefabRuntimeBehavior, UE
//      ALevelInstance::DesiredRuntimeBehavior - the instance record's override, else the prefab's own):
//      PARTITIONED - the body's records directly under the root are unparented from it - their transform becomes
//      the root's composed with their own (Rules::Detail::ComposeLocal, the planner's convention) and they hang
//      off the instance's own parent - so each is placed by its own bounds; a nested instance lifted so whose
//      own behaviour is Partitioned opens the same way. Deeper links are real containment and are kept. As in UE
//      embedded mode, moving a Partitioned instance's root at runtime does not carry its body.
//      STANDALONE (UE LevelStreaming) - nothing is unparented: the body stays under the root, so the planner's
//      containment makes the whole instance ONE composite (footprint = the union of its body's), it lands in
//      one cell, that cell's HLOD stands in for all of it, and moving the root at runtime carries the body.
// No expanded record carries a PrefabPath, PrefabOverrides or RuntimeBehavior; a record that states a
// RuntimeBehavior and instances no prefab is refused by name (it would be a setting nothing reads).

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/PrefabReader.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Core::WorldCells
{
    struct ExpandedSlot
    {
        std::size_t First    = 0; // index into ExpandedScene::Scene.Entities
        std::size_t Count    = 0; // 1 for a record that is no instance
        bool        Instance = false;
        // An instance's resolved runtime behaviour (its record's override, else its prefab's); Partitioned for a
        // record that is no instance.
        Assets::PrefabRuntimeBehavior Behavior = Assets::PrefabRuntimeBehavior::Partitioned;
    };

    struct ExpandedScene
    {
        SceneSerialized           Scene; // the source scene with every instance replaced by its records
        std::vector<ExpandedSlot> Slots; // one per SOURCE record, in source order
        std::size_t               Instances  = 0;
        std::size_t               Standalone = 0; // of Instances, those that stream whole
    };

    // Refused, naming the record and the prefab: an instance with no id or no stated transform, an empty reader
    // while the scene has an instance, a prefab that does not read, is empty, repeats an id or nests itself, an
    // override that misses, an expanded id that is null or collides with another record's.
    [[nodiscard]] Common::ResultStr<ExpandedScene> ExpandPrefabInstances( const SceneSerialized& scene,
                                                                          const PrefabReader&    reader );

    // The reader a cook gives: the stated path resolved under the assets root (Assets::ResolvePrefabPath), the
    // file read whole and parsed as the loader parses it (Assets::ParseLoadablePrefab). Refused by path: a file
    // that does not open, a prefab that does not parse.
    [[nodiscard]] PrefabReader PrefabFileReader();

    // THE WORLD A COOK CUTS (WP21): @p scene with every prefab instance expanded (ExpandPrefabInstances) and the
    // descriptors it is planned with - the source's own for a plain record, the expanded records' own
    // (DescribeEntities) in place of an instance's. CookWorld and BuildWorldHLODs both cut this world, so the
    // HLODs Build HLODs saves are the ones the cook checks. Refused when @p descriptors are not the source
    // records', id for id and in order, or when an instance does not expand (naming the prefab).
    struct ExpandedWorld
    {
        ExpandedScene                        World;
        std::vector<Rules::EntityDescriptor> Descriptors; // one per World.Scene record
    };
    [[nodiscard]] Common::ResultStr<ExpandedWorld>
    ExpandWorldForCook( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
                        const PrefabReader& prefabs );
} // namespace Desert::Core::WorldCells

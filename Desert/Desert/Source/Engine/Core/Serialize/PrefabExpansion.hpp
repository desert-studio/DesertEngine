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
//   4. THE SPLIT: the body's records directly under the root are unparented from it - their transform becomes
//      the root's composed with their own (Rules::Detail::ComposeLocal, the planner's convention) and they hang
//      off the instance's own parent - so each is placed by its own bounds. Deeper links are real containment
//      and are kept. Consequence (UE embedded mode has it too): in a cooked world, moving an instance root at
//      runtime does not carry its body; a prefab that must move whole needs an author flag (follow-up card).
// No expanded record carries a PrefabPath or PrefabOverrides.

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

namespace Desert::Core::WorldCells
{
    // Reads the prefab a record states (EntityData::PrefabPath spelling). The tool and the packager read the file
    // (Assets::ResolvePrefabPath + Assets::ParseLoadablePrefab); a suite answers from a table.
    using PrefabReader = std::function<Common::ResultStr<Assets::PrefabData>( std::string_view statedPath )>;

    struct ExpandedSlot
    {
        std::size_t First    = 0; // index into ExpandedScene::Scene.Entities
        std::size_t Count    = 0; // 1 for a record that is no instance
        bool        Instance = false;
    };

    struct ExpandedScene
    {
        SceneSerialized           Scene; // the source scene with every instance replaced by its records
        std::vector<ExpandedSlot> Slots; // one per SOURCE record, in source order
        std::size_t               Instances = 0;
    };

    // Refused, naming the record and the prefab: an instance with no id or no stated transform, an empty reader
    // while the scene has an instance, a prefab that does not read, is empty, repeats an id or nests itself, an
    // override that misses, an expanded id that is null or collides with another record's.
    [[nodiscard]] Common::ResultStr<ExpandedScene> ExpandPrefabInstances( const SceneSerialized& scene,
                                                                          const PrefabReader&    reader );
} // namespace Desert::Core::WorldCells

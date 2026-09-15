#pragma once

// READING AN INSTANCE BACK OUT: what does this prefab instance hold that its source file does not?
//
// This is the write half of the override mechanism; PrefabFactory::ApplyOverrides is the read half, and
// the pure comparison both rest on is Assets::DiffPrefabEntity. It lives here rather than in
// PrefabAsset because it needs the LIVE scene (an entity, its subtree, the entity serializer) and the
// asset manager to reach a nested prefab's own records — PrefabAsset has neither and should not grow
// them.
//
// EVERY ANSWER IT CANNOT GIVE IS COUNTED. Three things happen inside a prefab instance that a diff
// against the source cannot express — an entity added, a component removed, an entity of the source that
// is no longer there — and before this existed all three had the same outcome as everything else: the
// edit was dropped and nothing said so. They are counted here and named by the caller, which is the
// difference between a known limit and a defect.

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/ECS/Entity.hpp>

#include <cstddef>
#include <vector>

namespace Desert::Core::Serialize
{
    struct PrefabInstanceCapture
    {
        std::vector<Assets::PrefabOverrideData> Overrides;

        // Entities under the instance root that PrefabFactory did not create — the user added them after
        // instantiating. They carry no record address, so nothing in the file can name them.
        std::size_t AddedEntities = 0;

        // Components the source record carries that the live entity no longer does (see PrefabDiffReport).
        std::size_t RemovedComponents = 0;

        // Entities whose record address resolves to no record of the source prefab — an id-less record in
        // a hand-written `.deprefab` (PlanSceneStitch mints those a fresh id per load, so they have no
        // stable address), or a prefab that was re-authored under the instance's feet.
        std::size_t UnaddressableEntities = 0;
    };

    // Compare the live subtree at @p instanceRoot against the prefab it was instantiated from.
    //
    // @p instanceRoot must carry a PrefabComponent naming a loadable prefab; anything else returns an
    // empty capture, because an entity that is not an instance has nothing to differ from.
    [[nodiscard]] PrefabInstanceCapture CapturePrefabInstance( ECS::Entity                 instanceRoot,
                                                               const Assets::AssetManager& assetManager );
} // namespace Desert::Core::Serialize

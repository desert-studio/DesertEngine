#pragma once

// WHAT AN INSTANCE KEEPS OF ITS OWN, as a pure function of two parsed records.
//
// The capture that walks a live scene and the loader that puts the values back both need to agree on one
// question — "does this entity differ from the prefab it came from, and where?" — and the two sides of
// that question are the classic place this project loses properties: a diff that records more than it
// should pins fields the source should still own, and one that records less drops a user's edit without
// saying so. So it is ONE comparison, here, with no ECS, no filesystem and no asset manager in it, which
// is also what lets a suite hold every case without a GPU.
//
// SEE PrefabOverrideData (PrefabData.hpp) for why overrides are a diff rather than a copy, and for what
// `Path` addresses.

#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets
{
    // What the comparison could NOT express, counted rather than discarded. A caller logs it by name;
    // silence here would be the same defect the overrides exist to fix, one level up.
    struct PrefabDiffReport
    {
        // Component keys the base record carries and the live entity no longer does. An override is
        // applied ON TOP of the instantiated base, so "this component was removed from this instance"
        // has no spelling in it — the base's copy would come back on the next load.
        std::size_t RemovedComponents = 0;
    };

    // The difference between @p live and the prefab record @p base it was instantiated from, addressed by
    // @p path. `std::nullopt` means they are identical, which is what the file should then say: an empty
    // override record is a pinned copy of nothing and still costs the reader a line.
    //
    // FLOATS ARE COMPARED EXACTLY, and that is deliberate. Both sides of every comparison here have been
    // through the same writer — the base was parsed from JSON this engine wrote, the live value was put
    // there by that same parse — so "equal" means bit-equal for anything untouched, and a tolerance would
    // only serve to swallow small real edits.
    [[nodiscard]] std::optional<PrefabOverrideData> DiffPrefabEntity( const EntityData&         base,
                                                                      const EntityData&         live,
                                                                      std::vector<Common::UUID> path,
                                                                      PrefabDiffReport*         report = nullptr );

    // The override in the shape EntitySerializer::DeserializeEntity already consumes, so applying one to a
    // LIVE ENTITY is the same code path as loading an entity and cannot drift from it.
    [[nodiscard]] EntityData OverrideAsEntityData( const PrefabOverrideData& over );

    // The same override applied to a RECORD instead of an entity — what the instance was born holding,
    // which is the base a later comparison has to be made against (a nested prefab's own overrides are
    // laid over its records before the outer instance is diffed, or they would be discovered again and
    // pinned twice).
    //
    // THE TWO APPLIERS MUST AGREE, and the suite holds them to it: laying an override over a record must
    // produce the same values as handing OverrideAsEntityData's result to the entity deserializer. Two
    // ways to apply one thing is this project's most expensive defect shape.
    void LayerOverrideOntoRecord( EntityData& base, const PrefabOverrideData& over );

    // A path as one string, for use as a map key. Not a display form: ids are decimal and separated by
    // '/', so two different paths cannot collide.
    [[nodiscard]] std::string PrefabPathKey( std::span<const Common::UUID> path );
} // namespace Desert::Assets

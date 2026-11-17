#pragma once

// ONE REMAP OF A PREFAB BODY'S ENTITY LINKS, FOR EVERY PLACE A BODY BECOMES ENTITIES OR RECORDS.
//
// A record states the entities it references BY ID - the ids of its own file (Rules::kEntityReferences: a
// SocketAttachment's Target, a Projectile's Owner, a LandscapeTile's Landscape). Three places turn a body into
// something whose ids are not the file's:
//   * the cook (WorldCells::ExpandPrefabInstances) gives every record a stable hashed id;
//   * the factory (Runtime::Factory::PrefabFactory::Instantiate) mints a fresh uuid per entity;
//   * the capture (Assets::PrefabAsset::CreateFromEntity) writes a live subtree back under its record ids.
// Each one rewrites those fields through ITS map. The factory used not to: an attachment inside a prefab kept
// the record id, which names no entity of the instance, so the editor and Play hung it nowhere (or on the entity
// the prefab was cut from) while the cooked world hung it on its socket. One function and one reading of the
// field (the whole text is the decimal id std::to_string wrote), so the three cannot drift apart.

#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <functional>
#include <optional>

namespace Desert::Core::Rules
{
    // The id a stated reference becomes, or nullopt to leave the field as written: a target outside the body's
    // own file is not resolved by the cook nor by the factory (ids are per file), by the same rule.
    using EntityIdRemap = std::function<std::optional<Common::UUID>( const Common::UUID& stated )>;

    // Rewrites every Rules::kEntityReferences field of @p record through @p remap and answers how many changed.
    // A field whose text is not a whole decimal id is left alone - the World Partition planner reports that one.
    // The record's `parent` is structure, not a reference field (see kEntityReferences), and is not touched.
    std::size_t RemapEntityReferences( Assets::EntityData& record, const EntityIdRemap& remap );

    // True when @p record carries a component block that can hold an entity reference, so a caller that would
    // have to copy the record to remap it copies only the records that need it.
    [[nodiscard]] bool HasEntityReferenceBlock( const Assets::EntityData& record );
} // namespace Desert::Core::Rules

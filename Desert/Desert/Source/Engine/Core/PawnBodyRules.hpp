#pragma once

// The pure half of Core::DefaultPawnCapsule: which block of a prefab's records is its pawn's body. Its own header
// so PlayerStart.hpp's many includers do not pay for the JSON tree; compiled in PlayerStartRules.cpp, so a test
// runs it on records it wrote, with no asset manager behind it.
#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Core
{
    // A prefab's records by the path a nesting record names (EntityData::PrefabPath); refuses with the reason.
    using NestedPrefabRecords =
         std::function<Common::ResultStr<const std::vector<Assets::EntityData>*>( const std::string& path )>;

    // THE "CharacterController" BLOCK A PREFAB'S INSTANCE WOULD CARRY, read from its records without instancing
    // it (UE: the CDO, inherited and nested components included): the first entity in hierarchy order that has
    // one. A nesting record (PrefabPath) is a link, not an entity (PrefabRecordPolicy::InstantiatedLater): the
    // nested file's body is walked where the record stands, through @p nested, and the overrides that address
    // an entity inside it - the nesting record's own, then the enclosing prefabs', the order PrefabFactory
    // applies them - are merged onto that entity's block (MergePayload), or ARE its block when the entity had
    // none. A prefab that nests itself, or a nested prefab @p nested refuses, is an error naming the path.
    // nullopt = no entity has a controller (a spectator pawn).
    [[nodiscard]] Common::ResultStr<std::optional<Common::Json::Value>>
    PawnControllerBlock( const std::vector<Assets::EntityData>& records, const NestedPrefabRecords& nested );
} // namespace Desert::Core

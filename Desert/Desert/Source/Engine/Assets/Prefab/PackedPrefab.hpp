#pragma once

// THE PACKED FORM OF A PREFAB (UE 5.5 APackedLevelActor + FPackedLevelActorBuilder / FPackedLevelActorISMBuilder).
//
// A prefab of 200 fence sections placed ten times is 2 000 entities the ECS walks every frame and 2 000 static
// mesh proxies. Packed, it is one instanced draw per distinct (mesh, material slots, shadow flags): the instance
// is its root plus one InstancedStaticMesh child per group, the instances composed in the prefab ROOT's frame.
//
// ── WHY ONE DERIVED FORM PER PREFAB AND NOT A FOLD PER INSTANCE ────────────────────────────────────────────
//
// UE builds the packed form once, as a Blueprint beside the source level, and rebuilds it when the source level
// changes; every packed actor of it draws that one form. The alternative - running the fold on each instance as
// it is placed - would make ten instances ten foldings of the same records and give an edit of the source no
// single place to land. Here the form is a block of the `.deprefab` itself (PrefabData::Packed), written by the
// one prefab writer from the body it writes, and stamped with the checksum of that body (PrefabSourceHash): a
// file whose body no longer matches its stated form (edited outside the editor, written by an older build) is
// repacked when it is read. Every packed instance holds the form it was built from and follows the asset's
// current one (PrefabFactory::RefreshPackedInstances), so a Commit of the source reaches all of them.
//
// ── WHAT GROUPS, WHAT REFUSES ─────────────────────────────────────────────────────────────────────────────────
//
// The grouping key is the fold's (Editor/Core/Commands/InstanceFold.hpp, FoldMeshIdentity): mesh, material
// slots, primitive, CastShadows, CastContactShadow - the fields an ISM carries for all of its instances. What a
// static mesh carries and an ISM cannot is the fold's list too, StaticMeshPackBlockers below, which the editor's
// Collapse and this packer both ask. UE packs what it can and logs the rest away; a packed instance that quietly
// lost a light, a script or a forced LOD is the middle-link defect this project keeps finding, so the packer
// REFUSES the whole prefab, naming every entity and what it carries (PackedPrefabSer::Refused), as the fold
// does. Such a prefab is placed unpacked, and the Details panel says why it cannot pack.
//
// A NESTED PREFAB IS REFUSED BY NAME. UE's RecursiveBuilder flattens a nested level instance into the outer
// form; here the outer form's checksum would then have to cover every nested file as well, which is the
// remainder this card states (REMAINDER, WP-PACKED-PREFAB).
//
// PURE: a function of the records, so the writer, the reader, a suite and the migrator get one answer.

#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets
{
    // What a static mesh carries that an instanced static mesh cannot (the authored component's fields, read off
    // a live StaticMeshComponent by the editor's Collapse and off a record's StaticMesh block by the packer).
    // Defaults are the component's.
    struct StaticMeshPackFacts
    {
        bool     OutlineDraw              = false;
        int      ForcedLOD                = -1;
        int      LODBias                  = 0;
        bool     ReceiveShadows           = true;
        int      TranslucencySortPriority = 0;
        uint64_t HiddenSubmeshes          = 0;
        float    MaxDrawDistance          = 0.0f; // cm
        bool     EditMesh                 = false;
    };

    // Each knob somebody moved that an ISM has no room for, worded for a refusal ("a forced LOD"). Empty when the
    // mesh folds/packs. THE ONE LIST: InstanceFold's editor half and PackPrefab both ask it.
    [[nodiscard]] std::vector<std::string> StaticMeshPackBlockers( const StaticMeshPackFacts& facts );

    // The checksum of a prefab body (its records, as the one writer spells them): FNV-1a 64 of the JSON text, as
    // 16 hex digits. The packed form states the checksum of the body it was built from.
    [[nodiscard]] std::string PrefabSourceHash( std::span<const EntityData> records );

    // The packed form of a prefab body whose root is its FIRST record (PrefabFactory's rule). Groups in the order
    // their first source appears, instances in record order, each in the root's frame. Refused (with every
    // entity named) when any record cannot be packed or when nothing in the body draws a static mesh.
    [[nodiscard]] PackedPrefabSer PackPrefab( std::span<const EntityData> records );

    // How many instances the groups of @p packed hold together.
    [[nodiscard]] std::size_t PackedInstanceCount( const PackedPrefabSer& packed );
} // namespace Desert::Assets

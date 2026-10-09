#pragma once

// HOW A MESH ENTITY TAKES ITS MESH ASSET'S MATERIALS — PER SECTION (UE: UMeshComponent::GetMaterial, where an
// empty OverrideMaterials entry means "the mesh's own material for this section").
//
// A NULL slot is "this section's material comes from the mesh asset". Each one is filled independently the
// moment its material resolves; one that does not resolve yet stays Null — the renderer draws the default
// surface for THAT section only — and is tried again next time, so a material that registers later than
// the mesh replaces the default as soon as it does.
//
// It replaced an all-or-nothing rule (MeshECSSystem::AdoptMeshMaterialSlots): one unresolved .demat left
// every slot empty, so a single missing material (the cypress's Foliage_Trunk.demat) whitened the WHOLE
// mesh for as long as that one material stayed missing.

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Desert::ECS
{
    // @p slots: the component's material slots (an empty list is sized to the mesh's sections, all Null).
    // @p sectionMaterials: the mesh asset's material id per section.
    // @p resolve: section material id -> the material service's handle, Null while it is not registered.
    // Returns true when a slot changed (sized or filled), i.e. the cached material instances must be rebuilt.
    template <typename Resolve>
    bool AdoptSectionMaterials( std::vector<Common::AssetHandle>& slots,
                                const std::vector<Common::UUID>& sectionMaterials, Resolve&& resolve )
    {
        bool changed = false;
        if ( slots.empty() && !sectionMaterials.empty() )
        {
            slots.resize( sectionMaterials.size() );
            changed = true;
        }
        const size_t sections = std::min( slots.size(), sectionMaterials.size() );
        for ( size_t i = 0; i < sections; ++i )
        {
            if ( !slots[i].IsNull() || sectionMaterials[i].IsNull() )
                continue;
            const Common::AssetHandle resolved = resolve( sectionMaterials[i] );
            if ( resolved.IsNull() )
                continue;
            slots[i] = resolved;
            changed  = true;
        }
        return changed;
    }

    // Whether AdoptSectionMaterials can still change anything: the list is empty or holds a Null slot.
    // The per-frame gate, so an entity whose every section is resolved never looks its mesh asset up.
    inline bool HasUnadoptedSlot( const std::vector<Common::AssetHandle>& slots )
    {
        return slots.empty() || std::any_of( slots.begin(), slots.end(),
                                             []( const Common::AssetHandle& h ) { return h.IsNull(); } );
    }
} // namespace Desert::ECS

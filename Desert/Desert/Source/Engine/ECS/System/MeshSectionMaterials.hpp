#pragma once

#include <Engine/Assets/Common.hpp>

#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <vector>

namespace Desert::ECS
{
    // What one pass of FillSectionMaterials did: whether any slot changed (the caller rebuilds its runtime
    // instances on it), and which sections name a mesh material that does not resolve yet.
    struct SectionMaterialFill
    {
        bool                     Changed = false;
        std::vector<std::size_t> Unresolved;
    };

    // THE SECTION RULE, PER SECTION (UE: UMeshComponent::GetMaterial(i) answers the component's override when
    // it has one and the mesh's section material otherwise; a section whose material is missing draws
    // UMaterial::GetDefaultMaterial while the other sections draw their own). A component slot that is
    // Null takes the mesh's material for that index; a mesh material the resolver cannot answer yet leaves
    // ONLY its slot Null - that section draws the default surface, and the next pass retries it, so a
    // material registered after the mesh still arrives. A component with no slot at all takes one slot per
    // mesh material. Slots the component names itself are never touched.
    //
    // `resolve` maps the mesh's external material id to the material service's handle (Null when the content
    // registry has no row for it).
    template <typename Resolve>
    SectionMaterialFill FillSectionMaterials( std::vector<Assets::AssetHandle>& slots,
                                              const std::vector<Common::UUID>& meshMaterials, Resolve&& resolve )
    {
        SectionMaterialFill fill;
        if ( slots.empty() && !meshMaterials.empty() )
        {
            slots.assign( meshMaterials.size(), Assets::AssetHandle{} );
            fill.Changed = true;
        }
        const std::size_t sections = slots.size() < meshMaterials.size() ? slots.size() : meshMaterials.size();
        for ( std::size_t i = 0; i < sections; ++i )
        {
            if ( !slots[i].IsNull() || meshMaterials[i].IsNull() )
                continue;
            const Assets::AssetHandle internal = resolve( meshMaterials[i] );
            if ( internal.IsNull() )
            {
                fill.Unresolved.push_back( i );
                continue;
            }
            slots[i]     = internal;
            fill.Changed = true;
        }
        return fill;
    }
} // namespace Desert::ECS

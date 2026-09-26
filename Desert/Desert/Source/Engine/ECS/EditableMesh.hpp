#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    class DynamicMesh3;
}

namespace Desert::ECS
{
    struct StaticMeshComponent;

    // THE ONE WRITER OF A STATIC MESH'S EDITOR-BUILT GEOMETRY.
    //
    // StaticMeshComponent::EditableMesh is the source of truth and RuntimeMesh is derived from it. Two
    // fields that must agree are only safe if exactly one function writes both, so every tool, the scene
    // loader and the undo stack go through these two and nothing assigns either field directly. Before M4
    // the tools wrote the render buffer itself (CubeGrid replaced it, PolyEdit edited its vertices in
    // place), and the scene saved whatever that buffer held - three spellings of one mesh, with the saved
    // one losing tangents and submeshes.
    //
    // Sets the source and rebuilds the render mesh from it (Geometry::ToRenderMesh, UV layer 0 -> TexCoord,
    // one submesh per MaterialID). REFUSED, with the component untouched, when the conversion refuses (a
    // live triangle unset in a carried layer), when the mesh has no triangle (there is nothing to draw, and
    // an empty render mesh would leave the entity invisible with no word), or when the upload fails.
    [[nodiscard]] Common::BoolResultStr SetEditableMesh( StaticMeshComponent&                          component,
                                                         std::shared_ptr<const Geometry::DynamicMesh3> mesh );

    // Grows `slots` so every id in `materialIds` (one per render submesh, ascending) names a slot; a slot
    // it adds is EMPTY - handle 0, the default material, which is what the renderer draws for a missing slot
    // anyway. Never shrinks: a slot the user assigned outlives an edit that stops using it. SetEditableMesh
    // calls it on every write, so a component never says "0 slots" while its triangles use slot 0 - the state
    // in which Output / Convert to Static Mesh refused every Cube Grid and Create Shape result ("triangle 0
    // uses material slot 0, the asset has 0 slots"). Header-only so a suite without a device can run it.
    // True when it added a slot.
    [[nodiscard]] inline bool CoverMaterialIds( std::vector<Common::AssetHandle>& slots,
                                                std::span<const int>              materialIds )
    {
        if ( materialIds.empty() || materialIds.back() < 0 )
            return false;
        const auto needed = static_cast<std::size_t>( materialIds.back() ) + 1;
        if ( slots.size() >= needed )
            return false;
        slots.resize( needed );
        return true;
    }

    // Drops both: the entity falls back to its asset handle or primitive. What every "the mesh is now
    // something else" path calls (a primitive picked in Details, a photogrammetry result, a preview reset).
    void ClearEditableMesh( StaticMeshComponent& component );
} // namespace Desert::ECS

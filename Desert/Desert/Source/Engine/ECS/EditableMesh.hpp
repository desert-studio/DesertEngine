#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include <entt/entt.hpp>

namespace Desert::Geometry
{
    class DynamicMesh3;
}

namespace Desert::ECS
{
    struct StaticMeshDesc;

    // THE ONE WRITER OF A STATIC MESH'S EDITOR-BUILT GEOMETRY.
    //
    // StaticMeshComponent::EditableMesh is the source of truth and RuntimeMesh is derived from it. Two
    // fields that must agree are only safe if exactly one function writes both, so every tool, the scene
    // loader and the undo stack go through these and nothing assigns either field directly. Before M4
    // the tools wrote the render buffer itself (CubeGrid replaced it, PolyEdit edited its vertices in
    // place), and the scene saved whatever that buffer held - three spellings of one mesh, with the saved
    // one losing tangents and submeshes.
    //
    // Sets the source and rebuilds the render mesh from it (Geometry::ToRenderMesh, UV layer 0 -> TexCoord,
    // one submesh per MaterialID). REFUSED, with the component untouched, when the conversion refuses (a
    // live triangle unset in a carried layer), when the mesh has no triangle (there is nothing to draw, and
    // an empty render mesh would leave the entity invisible with no word), or when the upload fails.
    //
    // On a VALUE (a StaticMeshDesc being built or edited before it is handed to ECS::SetStaticMesh) ...
    [[nodiscard]] Common::BoolResultStr SetEditableMesh( StaticMeshDesc&                               desc,
                                                         std::shared_ptr<const Geometry::DynamicMesh3> mesh );
    // ... and on an entity's component: copies its value, sets the mesh on it and ends in ECS::SetStaticMesh,
    // so on_update<StaticMeshComponent> fires. Refused, with the component untouched, as above; also refused
    // when the entity has no StaticMeshComponent.
    [[nodiscard]] Common::BoolResultStr SetEditableMesh( entt::registry& registry, entt::entity entity,
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
    void ClearEditableMesh( StaticMeshDesc& desc );
    // The same on an entity's component, through ECS::SetStaticMesh (no write when it holds none).
    void ClearEditableMesh( entt::registry& registry, entt::entity entity );

    // UE's UStaticMeshComponent::PostEditChangeProperty for the two reflected references. `edited` is the
    // component's value after an edit (DesertCtl `set` and its undo and redo, through the entry's EditData);
    // it is compared with the entity's component and written through ECS::SetStaticMesh, so
    // on_update<StaticMeshComponent> fires and Core::Scene's render-state dirty set sees the swap (UE
    // MarkRenderStateDirty). An edit that changed neither reference is no write. Returns whether either changed.
    //   - a new MeshHandle IS the mesh: the Primitive and the edited mesh that would take priority over it in
    //     the draw are dropped (UE SetStaticMesh);
    //   - a changed MaterialSlots drops the runtime instances (SetStaticMesh does), so the draw builds them
    //     again from the slots.
    bool ApplyStaticMeshEdit( entt::registry& registry, entt::entity entity, StaticMeshDesc edited );

    // The same for an edit drawn IN PLACE on the live component (the Details mesh row: the generic property
    // drawer writes the reflected member where it lives and records its byte undo there): `before` is the
    // value before the draw. The edit is taken back out of the component through ECS::SetStaticMesh and
    // re-stated through ApplyStaticMeshEdit, so the change reaches the dirty set only through the writer.
    bool PostEditStaticMesh( entt::registry& registry, entt::entity entity, const StaticMeshDesc& before );
} // namespace Desert::ECS

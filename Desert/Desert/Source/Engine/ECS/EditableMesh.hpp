#pragma once

#include <Common/Core/ResultStr.hpp>

#include <memory>

namespace Desert::Geometry
{
    class FDynamicMesh3;
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
    [[nodiscard]] Common::BoolResultStr SetEditableMesh( StaticMeshComponent&                           component,
                                                         std::shared_ptr<const Geometry::FDynamicMesh3> mesh );

    // Drops both: the entity falls back to its asset handle or primitive. What every "the mesh is now
    // something else" path calls (a primitive picked in Details, a photogrammetry result, a preview reset).
    void ClearEditableMesh( StaticMeshComponent& component );
} // namespace Desert::ECS

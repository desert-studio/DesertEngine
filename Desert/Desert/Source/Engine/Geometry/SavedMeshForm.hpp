#pragma once

#include <optional>
#include <vector>

namespace Desert::Geometry
{
    // THE SAVED FORM OF AN EDITED MESH: what a scene (StaticMesh.EditMesh) and a prefab store for a mesh built in
    // the editor, instead of the render vertices they stored before schema v22.
    //
    // WHY NOT THE RENDER ARRAYS. The render side is DERIVED (EditMeshConversion.hpp): it has no topology, no
    // polygroups, no colour layer and one UV channel, and it duplicates a vertex at every seam. A scene that
    // stored it lost all of that on every save and had to re-weld by distance on every load; the v21 form
    // lost even more - Position, Normal and TexCoord only, so tangents came back uninitialised, and every
    // submesh collapsed into one. Storing the EditMesh stores the source of truth, and the render mesh is
    // rebuilt from it by the same conversion the editor used before the save.
    //
    // WHAT IS STORED IS EXACTLY THE STATE, NOTHING DERIVED. Positions, triangles, the per-triangle polygroup
    // and material ID, and each ENABLED overlay as its elements plus three element indices per triangle
    // (-1 on all three = the triangle is unset in that layer). An overlay's parent vertex is not stored: it
    // is implied by the first triangle that binds the element, which is how EditMeshOverlay itself sets it.
    // An absent overlay (nullopt) and an enabled-but-empty one are different states and stay different.
    //
    // IDS ARE COMPACTED ON WRITE. The saved form numbers vertices, triangles and elements densely in
    // ascending old-ID order (EditMesh::Compact), so a mesh with holes comes back with the same geometry
    // and attributes under renumbered IDs. That is safe because nothing that holds an ID survives a load:
    // the history is cleared with the scene, and a snapshot restore recreates the component.
    //
    // FLAT FLOAT ARRAYS, NOT glm. The block goes through reflect-cpp as JSON; a flat array keeps the text
    // one number per component and needs no glm reflector in the translation units that include this.
    // Every float is written as the double it widens to and read back to the same float, bit for bit.
    //
    // TWO CORES, ONE FORM. EditMeshSerialization and DynamicMeshSerialization both read and write this struct;
    // it lives on its own so the FDynamicMesh3 side does not include EditMesh. The field layout IS the scene
    // format (StaticMesh.EditMesh): changing it is a schema change.
    struct EditMeshOverlaySer
    {
        std::vector<float> Values;    // Components() floats per element
        std::vector<int>   Triangles; // 3 per triangle, in triangle order; -1 x3 = unset in this layer

        bool operator==( const EditMeshOverlaySer& ) const = default;
    };

    struct EditMeshSer
    {
        std::vector<float> Positions;   // xyz per vertex, cm
        std::vector<int>   Triangles;   // 3 vertex indices per triangle, winding a -> b -> c
        std::vector<int>   PolyGroups;  // one per triangle
        std::vector<int>   MaterialIds; // one per triangle

        std::optional<EditMeshOverlaySer> Normals;  // 3 floats per element
        std::optional<EditMeshOverlaySer> Tangents; // 4: xyz + bitangent sign
        std::optional<EditMeshOverlaySer> Colors;   // 4: linear RGBA
        std::vector<EditMeshOverlaySer>   UVs;      // 2 each, one entry per UV layer

        bool operator==( const EditMeshSer& ) const = default;
    };

} // namespace Desert::Geometry

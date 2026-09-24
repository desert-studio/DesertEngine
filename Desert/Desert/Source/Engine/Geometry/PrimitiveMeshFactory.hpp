#pragma once

#include <memory>
#include <Engine/Geometry/DynamicMesh.hpp>
#include "PrimitiveType.hpp"

namespace Desert::Geometry
{
    // A GPU CACHE, NOT A SOURCE OF GEOMETRY. The vertices of every primitive come from
    // Geometry::MakePrimitive (ShapeGenerators.hpp), the same generators the Modeling Create tool uses;
    // this class only uploads them and shares the result.
    class PrimitiveMeshFactory
    {
    public:
        // A fresh mesh of the primitive, nullptr for a type with no mesh here (Terrain, LightCube).
        static std::shared_ptr<DynamicMesh> Create( PrimitiveType type );

        // Returns a process-wide SHARED mesh for the given primitive type (created + GPU-invalidated once,
        // then cached). All non-edited primitive entities of the same type reference this single Mesh*, so
        // they batch via GPU instancing. The mesh editor forks a per-entity RuntimeMesh on edit
        // (copy-on-edit), so this shared mesh is never mutated. Returns nullptr for an unknown type.
        static DynamicMesh* GetShared( PrimitiveType type );

        // Drop the shared meshes. "Kept alive for the process" is exactly the problem: they own vertex and
        // index buffers, and a static destructor releases those after ~Application has destroyed the device
        // and the VMA allocator. Called from Renderer::Shutdown(), which runs inside main.
        static void ReleaseShared();
    };
} // namespace Desert::Geometry

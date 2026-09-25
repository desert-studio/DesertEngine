#include "PrimitiveMeshFactory.hpp"

#include <Engine/Geometry/ShapeGenerators.hpp>

#include <unordered_map>
#include <vector>

namespace Desert::Geometry
{
    std::shared_ptr<DynamicMesh> PrimitiveMeshFactory::Create( PrimitiveType type )
    {
        std::optional<ShapeMesh> shape = MakePrimitive( type );
        const auto               box   = PrimitiveBounds( type );
        if ( !shape.has_value() || !box.has_value() )
            return nullptr;

        // The submesh box is PrimitiveBounds, the statement the world partitioner reads as well; the
        // ShapeGenerators suite holds it to these vertices.
        const std::vector<Submesh> submeshes = {
             { PrimitiveTypeName( type ), 0, static_cast<uint32_t>( shape->Vertices.size() ), 0,
               static_cast<uint32_t>( shape->Indices.size() * 3 ), glm::mat4( 1.0f ), box.value() } };
        return std::make_shared<DynamicMesh>( shape->Vertices, shape->Indices, submeshes );
    }

    // One shared, GPU-ready mesh per primitive type, created lazily on first use and kept alive until
    // ReleaseShared(). Single-threaded engine, so no synchronization needed. At file scope rather than
    // inside GetShared() so that ReleaseShared() can reach it: as a function-local static it would only
    // ever have been destroyed at __cxa_finalize, which is after the device it holds buffers from.
    static std::unordered_map<PrimitiveType, std::shared_ptr<DynamicMesh>> s_SharedPrimitives;

    DynamicMesh* PrimitiveMeshFactory::GetShared( PrimitiveType type )
    {
        auto& mesh = s_SharedPrimitives[type];
        if ( !mesh )
        {
            mesh = Create( type );
            if ( mesh )
                mesh->Invalidate(); // build the GPU vertex/index buffers once
        }
        return mesh.get();
    }

    void PrimitiveMeshFactory::ReleaseShared()
    {
        s_SharedPrimitives.clear();
    }
} // namespace Desert::Geometry

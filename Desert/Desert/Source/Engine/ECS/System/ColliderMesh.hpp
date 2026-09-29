#pragma once

#include <Engine/ECS/Components.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Desert::ECS
{
    // The points and triangles a Mesh / ConvexHull collider is cooked from, in body space.
    struct ColliderMesh
    {
        std::vector<glm::vec3> Points; // world scale applied: the body's transform carries none
        std::vector<uint32_t>  Indices;
    };

    enum class ColliderMeshSource
    {
        None,
        RuntimeMesh,
        Primitive,
        Asset,
    };

    // UE's rule: the collider is the thing on screen, so its source is picked in the order MeshECSSystem draws
    // the StaticMesh — the edited mesh over the primitive over the asset.
    inline ColliderMeshSource PickColliderMeshSource( const StaticMeshComponent& mesh )
    {
        if ( mesh.RuntimeMesh )
            return ColliderMeshSource::RuntimeMesh;
        if ( mesh.Primitive.has_value() )
            return ColliderMeshSource::Primitive;
        if ( mesh.MeshHandle )
            return ColliderMeshSource::Asset;
        return ColliderMeshSource::None;
    }

    // Vertices carry `.Position`, triangles `.V1/.V2/.V3` — the shape shared by DynamicMesh and StaticMeshAsset.
    template <typename Vertices, typename Triangles>
    ColliderMesh BuildColliderMesh( const Vertices& vertices, const Triangles& triangles, const glm::vec3& scale )
    {
        ColliderMesh out;
        out.Points.reserve( vertices.size() );
        for ( const auto& v : vertices )
            out.Points.push_back( v.Position * scale );
        out.Indices.reserve( triangles.size() * 3u );
        for ( const auto& t : triangles )
            out.Indices.insert( out.Indices.end(), { t.V1, t.V2, t.V3 } );
        return out;
    }
} // namespace Desert::ECS

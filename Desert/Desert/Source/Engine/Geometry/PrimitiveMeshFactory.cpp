#include "PrimitiveMeshFactory.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <vector>
#include <unordered_map>

#include <Engine/Geometry/MeshFactory.hpp>
#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/Units.hpp>

namespace Desert::Geometry
{
    namespace
    {
        // Primitives are authored as a unit cube/sphere ([-0.5, 0.5]) and blown up to one METRE, the same
        // default size as a UE box: one world unit is a centimetre, so a "Cube" is 100 units on a side and
        // sits naturally next to a 100 cm CubeGrid block. See Common/Core/Units.hpp.
        constexpr float kPrimitiveSize = Common::Units::UnitsPerMetre;

        void ScaleToWorld( std::vector<Vertex>& vertices, Common::Math::AABB& aabb )
        {
            for ( Vertex& v : vertices )
                v.Position *= kPrimitiveSize;
            aabb.Min *= kPrimitiveSize;
            aabb.Max *= kPrimitiveSize;
        }
    } // namespace
    std::shared_ptr<DynamicMesh> PrimitiveMeshFactory::Create( PrimitiveType type )
    {
        switch ( type )
        {
            case PrimitiveType::Cube:
                return CreateCube();
            case PrimitiveType::Sphere:
                return CreateSphere();
            case PrimitiveType::Plane:
                return CreatePlane();
            case PrimitiveType::Pyramid:
                return CreatePyramid();
            default:
                return nullptr;
        }
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

    std::shared_ptr<DynamicMesh> PrimitiveMeshFactory::CreateCube()
    {
        std::vector<Vertex> vertices = {
            // Front face
            { { -0.5f, -0.5f,  0.5f }, { 0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { {  0.5f, -0.5f,  0.5f }, { 0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
            { {  0.5f,  0.5f,  0.5f }, { 0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            { { -0.5f,  0.5f,  0.5f }, { 0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
            // Back face
            { { -0.5f, -0.5f, -0.5f }, { 0.0f,  0.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
            { { -0.5f,  0.5f, -0.5f }, { 0.0f,  0.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            { {  0.5f,  0.5f, -0.5f }, { 0.0f,  0.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
            { {  0.5f, -0.5f, -0.5f }, { 0.0f,  0.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            // Top face
            { { -0.5f,  0.5f, -0.5f }, { 0.0f,  1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f } },
            { { -0.5f,  0.5f,  0.5f }, { 0.0f,  1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },
            { {  0.5f,  0.5f,  0.5f }, { 0.0f,  1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f } },
            { {  0.5f,  0.5f, -0.5f }, { 0.0f,  1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f } },
            // Bottom face
            { { -0.5f, -0.5f, -0.5f }, { 0.0f, -1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
            { {  0.5f, -0.5f, -0.5f }, { 0.0f, -1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
            { {  0.5f, -0.5f,  0.5f }, { 0.0f, -1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
            { { -0.5f, -0.5f,  0.5f }, { 0.0f, -1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            // Left face
            { { -0.5f, -0.5f, -0.5f }, { -1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -0.5f, -0.5f,  0.5f }, { -1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
            { { -0.5f,  0.5f,  0.5f }, { -1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            { { -0.5f,  0.5f, -0.5f }, { -1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
            // Right face
            { {  0.5f, -0.5f, -0.5f }, { 1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
            { {  0.5f,  0.5f, -0.5f }, { 1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            { {  0.5f,  0.5f,  0.5f }, { 1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
            { {  0.5f, -0.5f,  0.5f }, { 1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } }
        };

        std::vector<Index> indices = {
            { 0,  1,  2 },  { 2,  3,  0 },
            { 4,  5,  6 },  { 6,  7,  4 },
            { 8,  9,  10 }, { 10, 11, 8 },
            { 12, 13, 14 }, { 14, 15, 12 },
            { 16, 17, 18 }, { 18, 19, 16 },
            { 20, 21, 22 }, { 22, 23, 20 }
        };

        Common::Math::AABB cubeAABB;
        cubeAABB.Min = glm::vec3( -0.5f, -0.5f, -0.5f );
        cubeAABB.Max = glm::vec3(  0.5f,  0.5f,  0.5f );

        ScaleToWorld( vertices, cubeAABB );
        std::vector<Submesh> submeshes = {
            { "Cube", 0, (uint32_t)vertices.size(), 0, (uint32_t)indices.size() * 3, glm::mat4(1.0f), cubeAABB }
        };

        return std::make_shared<DynamicMesh>( vertices, indices, submeshes );
    }

    std::shared_ptr<DynamicMesh> PrimitiveMeshFactory::CreateSphere()
    {
        // UV sphere, radius 0.5 to match the cube's [-0.5, 0.5] extents. Winding is CCW when viewed from
        // outside (front face), consistent with CreateCube so back-face culling keeps the outer surface.
        constexpr uint32_t sectorCount = 32; // longitude divisions (theta: 0..2pi)
        constexpr uint32_t stackCount  = 16; // latitude divisions  (phi:   0..pi)
        constexpr float    radius      = 0.5f;

        std::vector<Vertex> vertices;
        vertices.reserve( static_cast<size_t>( stackCount + 1 ) * static_cast<size_t>( sectorCount + 1 ) );

        for ( uint32_t i = 0; i <= stackCount; ++i )
        {
            const float phi    = glm::pi<float>() * static_cast<float>( i ) / static_cast<float>( stackCount );
            const float sinPhi = std::sin( phi );
            const float cosPhi = std::cos( phi );

            for ( uint32_t j = 0; j <= sectorCount; ++j )
            {
                const float theta    = glm::two_pi<float>() * static_cast<float>( j ) / static_cast<float>( sectorCount );
                const float sinTheta = std::sin( theta );
                const float cosTheta = std::cos( theta );

                const glm::vec3 normal    = { sinPhi * cosTheta, cosPhi, sinPhi * sinTheta };
                const glm::vec3 position  = normal * radius;
                const glm::vec3 tangent   = { -sinTheta, 0.0f, cosTheta };
                const glm::vec3 bitangent = glm::cross( normal, tangent );
                const glm::vec2 uv = { static_cast<float>( j ) / static_cast<float>( sectorCount ),
                                       static_cast<float>( i ) / static_cast<float>( stackCount ) };

                vertices.push_back( { position, normal, tangent, bitangent, uv } );
            }
        }

        std::vector<Index> indices;
        indices.reserve( static_cast<size_t>( stackCount ) * static_cast<size_t>( sectorCount ) * 2 );

        const uint32_t stride = sectorCount + 1;
        for ( uint32_t i = 0; i < stackCount; ++i )
        {
            for ( uint32_t j = 0; j < sectorCount; ++j )
            {
                const uint32_t a = i * stride + j;
                const uint32_t b = a + stride;

                indices.push_back( { a, a + 1, b } );
                indices.push_back( { a + 1, b + 1, b } );
            }
        }

        Common::Math::AABB aabb;
        aabb.Min = glm::vec3( -radius );
        aabb.Max = glm::vec3( radius );

        ScaleToWorld( vertices, aabb );

        std::vector<Submesh> submeshes = {
            { "Sphere", 0, (uint32_t)vertices.size(), 0, (uint32_t)indices.size() * 3, glm::mat4( 1.0f ), aabb }
        };

        return std::make_shared<DynamicMesh>( vertices, indices, submeshes );
    }

    std::shared_ptr<DynamicMesh> PrimitiveMeshFactory::CreatePlane()
    {
        // Unit quad in the XY plane (normal +Z), centred at origin, extents [-0.5, 0.5]. UV (0,0) at the
        // bottom-left -> (1,1) top-right. Used as a flat "card" (foliage/decal material previews, billboards).
        constexpr glm::vec3 n( 0.0f, 0.0f, 1.0f );
        constexpr glm::vec3 t( 1.0f, 0.0f, 0.0f );
        const glm::vec3     b = glm::cross( n, t );

        std::vector<Vertex> vertices = {
             { { -0.5f, -0.5f, 0.0f }, n, t, b, { 0.0f, 0.0f } },
             { { 0.5f, -0.5f, 0.0f }, n, t, b, { 1.0f, 0.0f } },
             { { 0.5f, 0.5f, 0.0f }, n, t, b, { 1.0f, 1.0f } },
             { { -0.5f, 0.5f, 0.0f }, n, t, b, { 0.0f, 1.0f } },
        };

        // CCW when viewed from +Z (front face), matching the other primitives' winding.
        const std::vector<Index> indices = { { 0, 1, 2 }, { 0, 2, 3 } };

        Common::Math::AABB aabb;
        aabb.Min = glm::vec3( -0.5f, -0.5f, 0.0f );
        aabb.Max = glm::vec3( 0.5f, 0.5f, 0.0f );

        ScaleToWorld( vertices, aabb );

        std::vector<Submesh> submeshes = {
            { "Plane", 0, (uint32_t)vertices.size(), 0, (uint32_t)indices.size() * 3, glm::mat4( 1.0f ), aabb }
        };

        return std::make_shared<DynamicMesh>( vertices, indices, submeshes );
    }

    std::shared_ptr<DynamicMesh> PrimitiveMeshFactory::CreatePyramid()
    {
        // TODO: Implement actual pyramid vertex generation logic
        return nullptr;
    }

} // namespace Desert::Geometry
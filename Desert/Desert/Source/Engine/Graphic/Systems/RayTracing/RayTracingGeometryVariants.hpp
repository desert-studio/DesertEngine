#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace Desert
{
    class Mesh;
}

// THE BLAS OF A MESH THAT HIDES SOME SUBMESHES. UE keeps one BLAS per LOD and hides sections per instance through
// the hit group; an inline ray query has no hit group to ignore a segment, so the exact equivalent is a BLAS built
// without the hidden segments (DescribeStaticMeshGeometry already leaves them out). One variant per (mesh, hidden
// mask), built on the first frame a row asks for it and shared by every row with that mask; a variant no row
// asked for this frame is dropped at EndFrame. Dropping is safe while a frame in flight still traces it: the API
// object releases its acceleration structure and storage on the frame-retire ring (~VulkanRayTracingGeometry).
//
// A variant exists only for a mesh that has its own BLAS (Mesh::GetRayTracingGeometry): a mesh with none (not
// resident, no ray-tracing device, a kind the uploader builds none for) traces nothing, hidden or not.
namespace Desert::Graphic::RayTracing
{
    class RayTracingGeometryVariants
    {
    public:
        // Builds the BLAS of @p mesh without the @p hiddenMask submeshes. Null value: no BLAS by design (the
        // device traces no ray, nothing left to build). Tests inject a fake; production is MakeVariantBuild().
        using BuildFn =
             std::function<Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>>( Mesh&, uint64_t )>;

        explicit RayTracingGeometryVariants( BuildFn build ) : m_Build( std::move( build ) )
        {
        }

        // Mask 0: the mesh's own BLAS. Otherwise the (mesh, mask) variant of the mesh's CURRENT buffers (a
        // re-uploaded mesh gets a new variant, never the old one). Null value: the mesh has no BLAS. Error: the
        // build failed — answered once; the variant then stays empty (null) until the mesh's buffers change, so
        // the caller logs one line per (mesh, mask) and counts the row as NoGeometry every frame.
        [[nodiscard]] Common::ResultStr<const RayTracingGeometryResource*> Resolve( Mesh&    mesh,
                                                                                    uint64_t hiddenMask );

        // Drops every variant no Resolve asked for since the previous EndFrame.
        void EndFrame();

        [[nodiscard]] std::size_t LiveVariants() const
        {
            return m_Variants.size();
        }

    private:
        struct Key
        {
            const Mesh* MeshPtr = nullptr;
            uint64_t    Hidden  = 0;

            bool operator==( const Key& ) const = default;
        };
        struct KeyHash
        {
            std::size_t operator()( const Key& key ) const noexcept
            {
                return std::hash<const void*>{}( key.MeshPtr ) ^
                       ( std::hash<uint64_t>{}( key.Hidden ) * 0x9E3779B97F4A7C15ull );
            }
        };
        struct Variant
        {
            // The buffers the variant was built from: identity of the mesh's upload (weak, so an address reused by
            // a later buffer never matches a dead one).
            std::weak_ptr<void>                         Vertices;
            std::weak_ptr<void>                         Indices;
            std::shared_ptr<RayTracingGeometryResource> Geometry;
            bool                                        Used = false;
        };

        BuildFn                                   m_Build;
        std::unordered_map<Key, Variant, KeyHash> m_Variants;
    };

    // The production build: the API's blocking BLAS build of the mesh's resident buffers
    // (VulkanRayTracingGeometry:: BuildStatic with the hidden mask). Defined with the API object
    // (RayTracingSceneRenderer.cpp).
    [[nodiscard]] RayTracingGeometryVariants::BuildFn MakeVariantBuild();
} // namespace Desert::Graphic::RayTracing

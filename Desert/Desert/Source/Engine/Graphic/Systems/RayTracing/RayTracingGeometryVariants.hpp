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

// THE BLAS OF A MESH THAT HIDES SOME SUBMESHES OR HAS A MASKED SECTION. UE keeps one BLAS per LOD and hides
// sections per instance through the hit group; an inline ray query has no hit group to ignore a segment, so the
// exact equivalent is a BLAS built without the hidden segments (DescribeStaticMeshGeometry already leaves them
// out). A segment's opacity is a BLAS build flag (UE bakes each section's blend mode into its segment the same
// way), and the uploader builds the mesh's own BLAS all opaque, so a row whose section material is Masked traces a
// variant too. One variant per (mesh, RayTracingSubmeshMasks), built on the first frame a row asks for it and
// shared by every row with those masks: a section whose material changes blend mode changes the key and so gets
// its own build. A variant no row asked for this frame is dropped at EndFrame. Dropping is safe while a frame in flight still traces it: the API
// object releases its acceleration structure and storage on the frame-retire ring (~VulkanRayTracingGeometry).
//
// A variant exists only for a mesh that has its own BLAS (Mesh::GetRayTracingGeometry): a mesh with none (not
// resident, no ray-tracing device, a kind the uploader builds none for) traces nothing, hidden or not.
namespace Desert::Graphic::RayTracing
{
    class RayTracingGeometryVariants
    {
    public:
        // Builds the BLAS of @p mesh without the masks' hidden submeshes and with their non-opaque segments. Null
        // value: no BLAS by design (the device traces no ray, nothing left to build). Tests inject a fake;
        // production is MakeVariantBuild().
        using BuildFn = std::function<Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>>(
             Mesh&, const RayTracingSubmeshMasks& )>;

        explicit RayTracingGeometryVariants( BuildFn build ) : m_Build( std::move( build ) )
        {
        }

        // No mask set: the mesh's own BLAS. Otherwise the (mesh, masks) variant of the mesh's CURRENT buffers (a
        // re-uploaded mesh gets a new variant, never the old one). Null value: the mesh has no BLAS. Error: the
        // build failed — answered once; the variant then stays empty (null) until the mesh's buffers change, so
        // the caller logs one line per (mesh, masks) and counts the row as NoGeometry every frame.
        [[nodiscard]] Common::ResultStr<const RayTracingGeometryResource*>
        Resolve( Mesh& mesh, const RayTracingSubmeshMasks& masks );

        // Drops every variant no Resolve asked for since the previous EndFrame.
        void EndFrame();

        [[nodiscard]] std::size_t LiveVariants() const
        {
            return m_Variants.size();
        }
        // Storage bytes of the live variants (stats overlay).
        [[nodiscard]] uint64_t GetMemoryBytes() const
        {
            uint64_t bytes = 0;
            for ( const auto& item : m_Variants )
                if ( item.second.Geometry )
                    bytes += item.second.Geometry->GetMemoryBytes();
            return bytes;
        }

    private:
        struct Key
        {
            const Mesh*            MeshPtr = nullptr;
            RayTracingSubmeshMasks Masks;

            bool operator==( const Key& ) const = default;
        };
        struct KeyHash
        {
            std::size_t operator()( const Key& key ) const noexcept
            {
                return std::hash<const void*>{}( key.MeshPtr ) ^
                       ( std::hash<uint64_t>{}( key.Masks.Hidden ) * 0x9E3779B97F4A7C15ull ) ^
                       ( std::hash<uint64_t>{}( key.Masks.NonOpaque ) * 0xC2B2AE3D27D4EB4Full );
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
    // (VulkanRayTracingGeometry::BuildStatic with the masks). Defined with the API object
    // (RayTracingSceneRenderer.cpp).
    [[nodiscard]] RayTracingGeometryVariants::BuildFn MakeVariantBuild();
} // namespace Desert::Graphic::RayTracing

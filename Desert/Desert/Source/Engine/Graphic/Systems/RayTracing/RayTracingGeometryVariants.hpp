#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GeometryPool.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingOpacityMicromaps.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

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
// its own build. A variant no row asked for this frame is dropped at EndFrame. Dropping is safe while a frame in
// flight still traces it: the API object releases its acceleration structure and storage on the frame-retire ring
// (~VulkanRayTracingGeometry).
//
// A variant exists only for a mesh that has its own BLAS (Mesh::GetRayTracingGeometry): a mesh with none (not
// resident, no ray-tracing device, a kind the uploader builds none for) traces nothing, hidden or not.
//
// OPACITY MICROMAPS (RT-OMM-SER; UE 5.4 r.RayTracing.OpacityMicromaps): a variant's non-opaque segments are the
// only masked segments of a static mesh's BLAS, so the variant is where their micromaps live. Once per variant
// upload, on the frame its metadata first names their alpha test (QueueOpacityMicromaps, frame N), the scene
// renderer bakes them on the GPU (OpacityMicromapBake.shader); on the next frame's Resolve (N+1, the bake was
// submitted before on the same queue) the variant is rebuilt with them and the new BLAS replaces the old one -
// the same image, fewer any-hit candidates. A different alpha test for a baked segment (another texture or
// cutoff in the section's material) drops the micromaps and rebuilds the plain BLAS first: a stale micromap would
// answer OPAQUE where the new test lets the ray through. Without the catalog's OpacityMicromap nothing is queued.
namespace Desert::Graphic::RayTracing
{
    class RayTracingGeometryVariants
    {
    public:
        // Builds the BLAS of @p mesh without the masks' hidden submeshes and with their non-opaque segments. Null
        // value: no BLAS by design (the device traces no ray, nothing left to build). Tests inject a fake;
        // production is MakeVariantBuild().
        // @p micromaps: the baked micromaps of non-opaque segments (empty = a BLAS without micromaps).
        using BuildFn = std::function<Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>>(
             Mesh&, const RayTracingSubmeshMasks&, std::span<const RayTracingSegmentOpacityMicromap> micromaps )>;
        // The output buffers of one segment's bake (VulkanRayTracingGeometry::AllocateOpacityMicromapBake).
        using AllocateBakeFn =
             std::function<Common::ResultStr<RayTracingSegmentOpacityMicromap>( uint32_t segment, uint32_t triangles )>;

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

        // THIS FRAME'S BAKES: the @p candidates (CollectOpacityMicromapCandidates over the frame's metadata) of the
        // live variants that have no micromaps yet get their output buffers from @p allocate and are returned for
        // the frame's bake pass; the variant is rebuilt with them on the next frame's Resolve. A candidate whose
        // BLAS is no variant (a mesh's own BLAS, a procedural or skin-cache one) is not baked. Errors (an
        // allocation, a rebuild that failed) are reported once through TakeOpacityMicromapErrors; the variant then
        // keeps its plain BLAS.
        [[nodiscard]] std::vector<OpacityMicromapBake>
        QueueOpacityMicromaps( std::span<const OpacityMicromapCandidate> candidates, const AllocateBakeFn& allocate );

        struct OpacityMicromapStats
        {
            uint32_t BakedSegments  = 0; // segments of the live variants whose BLAS carries a micromap
            uint64_t BakedTriangles = 0;
            uint64_t BakedBytes     = 0; // the bake outputs (data + triangle rows + indices) of those segments
            uint32_t Failed         = 0; // variants whose bake or rebuild failed (plain BLAS kept)
        };
        [[nodiscard]] OpacityMicromapStats GetOpacityMicromapStats() const;
        [[nodiscard]] std::vector<std::string> TakeOpacityMicromapErrors()
        {
            return std::exchange( m_OpacityErrors, {} );
        }

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
                       ( std::hash<uint64_t>{}( key.Masks.NonOpaque ) * 0xC2B2AE3D27D4EB4Full ) ^
                       ( std::size_t{ key.Masks.Lod } * 0x165667B19E3779F9ull );
            }
        };
        struct Variant
        {
            // The buffers the variant was built from: identity of the mesh's upload (weak, so an address reused by
            // a later buffer never matches a dead one).
            std::weak_ptr<void>                         Vertices;
            std::weak_ptr<void>                         Indices;
            // Where in them (a pool arena holds many meshes; a re-upload moves the mesh to a new span).
            MeshGeometryBinding                         Binding;
            std::shared_ptr<RayTracingGeometryResource> Geometry;
            bool                                        Used = false;
            // RT-OMM-SER: None -> Baking (QueueOpacityMicromaps, frame BakeFrame) -> Built (Resolve, a later
            // frame) or Failed. A new upload resets it to None.
            enum class Opacity : uint8_t
            {
                None,
                Baking,
                Built,
                Failed,
            };
            Opacity                                       OpacityState = Opacity::None;
            uint64_t                                      BakeFrame    = 0;
            std::vector<RayTracingSegmentOpacityMicromap> Micromaps;   // the bake outputs, by segment
            std::vector<OpacityMicromapCandidate>         BakedFrom;   // the alpha test each was baked against
        };

        // Variant @p variant of @p key rebuilt with its baked micromaps (Resolve, the frame after the bake).
        void BuildWithMicromaps( Mesh& mesh, const RayTracingSubmeshMasks& masks, Variant& variant );

        BuildFn                                   m_Build;
        std::unordered_map<Key, Variant, KeyHash> m_Variants;
        uint64_t                                  m_Frame = 0; // EndFrame count: a bake of frame k builds at > k
        std::vector<std::string>                  m_OpacityErrors;
    };

    // The production build: the API's blocking BLAS build of the mesh's resident buffers
    // (VulkanRayTracingGeometry::BuildStatic with the masks). Defined with the API object
    // (RayTracingSceneRenderer.cpp).
    [[nodiscard]] RayTracingGeometryVariants::BuildFn MakeVariantBuild();
} // namespace Desert::Graphic::RayTracing

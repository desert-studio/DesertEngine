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

// THE SCENE'S BLAS OF EVERY COOKED MESH, BUILT ON DEMAND (UE r.RayTracing.Geometry: only the ray-tracing LOD a
// primitive instances is built, and it lives while something traces it). One entry per (mesh,
// RayTracingSubmeshMasks): masks {} is the mesh's own LOD-0 BLAS, any other key a variant. Nothing builds a BLAS
// ahead (the mesh uploader builds none): an entry is built on the first frame a traced row asks for it and released
// once no row asked for kUnusedFramesBeforeRelease frames, so a foliage field traced past LOD 0 holds no LOD-0 BLAS
// once no instance falls back to it. One owner: no copy of the BLAS lives on the mesh.
//
// A VARIANT: THE BLAS OF A MESH THAT HIDES SOME SUBMESHES OR HAS A MASKED SECTION. UE keeps one BLAS per LOD and
// hides sections per instance through the hit group; an inline ray query has no hit group to ignore a segment, so
// the exact equivalent is a BLAS built without the hidden segments (DescribeStaticMeshGeometry already leaves them
// out). A segment's opacity is a BLAS build flag (UE bakes each section's blend mode into its segment the same
// way), and the own BLAS (masks {}) is all opaque, so a row whose section material is Masked traces a variant too.
// Each entry is shared by every row with its key: a section whose material changes blend mode changes the key and
// so gets its own build. Dropping is safe while a frame in flight still traces it: the API object releases its
// acceleration structure and storage on the frame-retire ring (~VulkanRayTracingGeometry).
//
// No entry exists for a mesh whose buffers are not resident yet (it traces nothing this frame), for a skinned mesh
// (the skin cache builds its BLAS) or for a procedural one (Mesh::IsProceduralRayTracingGeometry: the RT scene's
// procedural cache builds it in the graph).
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

        // How many EndFrames an entry survives with no Resolve asking for it. Not a memory budget: hysteresis, so a
        // row that leaves the gather for a moment (a caster culled for a few frames, a foliage instance crossing
        // an RT-LOD boundary and back) traces its kept BLAS instead of paying a blocking rebuild; two seconds at
        // 60 Hz. Release itself is safe at any count (the frame-retire ring above).
        static constexpr uint64_t kUnusedFramesBeforeRelease = 120u;

        // The (mesh, masks) BLAS of the mesh's CURRENT buffers, built here on first ask (a re-uploaded mesh gets a
        // new build, never the old one); masks {} is the mesh's own LOD-0 BLAS. Null value: no BLAS this frame
        // (buffers not resident yet, a skinned or procedural mesh - see above). Error: the build failed - answered
        // once; the entry then stays empty (null) until the mesh's buffers change, so the caller logs one line per
        // (mesh, masks) and counts the row as NoGeometry every frame.
        [[nodiscard]] Common::ResultStr<const RayTracingGeometryResource*>
        Resolve( Mesh& mesh, const RayTracingSubmeshMasks& masks );

        // Ends a frame: drops every entry no Resolve asked for in the last kUnusedFramesBeforeRelease frames.
        void EndFrame();

        // THIS FRAME'S BAKES: the @p candidates (CollectOpacityMicromapCandidates over the frame's metadata) of the
        // live variants that have no micromaps yet get their output buffers from @p allocate and are returned for
        // the frame's bake pass; the variant is rebuilt with them on the next frame's Resolve. A candidate whose
        // BLAS is no entry here (a procedural or skin-cache one) is not baked; the own BLAS (masks {}) is all
        // opaque, so it names none. Errors (an allocation, a rebuild that failed) are reported once through
        // TakeOpacityMicromapErrors; the variant then keeps its plain BLAS.
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

        // The live (mesh, masks) BLAS without asking for it (no build, no use recorded: a stats reader); null when
        // none is resident (never built, released, or its build failed).
        [[nodiscard]] const RayTracingGeometryResource* FindResident( const Mesh&                   mesh,
                                                                      const RayTracingSubmeshMasks& masks ) const
        {
            const auto found = m_Variants.find( Key{ &mesh, masks } );
            return found != m_Variants.end() ? found->second.Geometry.get() : nullptr;
        }

        [[nodiscard]] std::size_t LiveVariants() const
        {
            return m_Variants.size();
        }
        // Storage bytes of the live entries, own LOD-0 BLASes included (stats overlay).
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
            // The mesh's render identity (never reused): an entry outlives the mesh by up to
            // kUnusedFramesBeforeRelease frames, and a new mesh built at the freed address may land in the same
            // pool span - the address and the buffers alone would hand it the dead mesh's BLAS.
            uint64_t                                    Revision = 0;
            std::shared_ptr<RayTracingGeometryResource> Geometry;
            uint64_t                                    LastUsedFrame = 0; // m_Frame of the last Resolve
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
        uint64_t                                  m_Frame = 0; // EndFrames so far: a bake of frame k builds at > k
        std::unordered_map<Key, Variant, KeyHash> m_Variants;
        std::vector<std::string>                  m_OpacityErrors;
    };

    // The production build: the API's blocking BLAS build of the mesh's resident buffers
    // (VulkanRayTracingGeometry::BuildStatic with the masks). Defined with the API object
    // (RayTracingSceneRenderer.cpp).
    [[nodiscard]] RayTracingGeometryVariants::BuildFn MakeVariantBuild();
} // namespace Desert::Graphic::RayTracing

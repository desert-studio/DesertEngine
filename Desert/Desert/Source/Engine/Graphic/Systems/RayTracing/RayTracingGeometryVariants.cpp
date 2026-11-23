#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometryVariants.hpp>

#include <Engine/Geometry/Mesh.hpp>

#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <format>

namespace Desert::Graphic::RayTracing
{
    Common::ResultStr<const RayTracingGeometryResource*>
    RayTracingGeometryVariants::Resolve( Mesh& mesh, const RayTracingSubmeshMasks& masks )
    {
        const std::shared_ptr<RayTracingGeometryResource>& own = mesh.GetRayTracingGeometry();
        if ( masks == RayTracingSubmeshMasks{} || !own )
            return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( own.get() ) );

        const MeshGeometry& geometry = mesh.GetGeometry();
        const auto          vertices = geometry.Vertices;
        const auto          indices  = geometry.Indices;
        const auto          binding  = geometry.MeshBinding();
        auto [found, inserted] = m_Variants.try_emplace( Key{ &mesh, masks } );
        Variant& variant       = found->second;
        variant.Used           = true;
        const bool sameUpload  = !inserted && variant.Vertices.lock().get() == vertices.get() &&
                                variant.Indices.lock().get() == indices.get() && variant.Binding == binding;
        if ( sameUpload )
        {
            // RT-OMM-SER: the bake QueueOpacityMicromaps returned ran in an earlier frame's graph, submitted before
            // anything this frame records on the same queue - its outputs are written for the build that reads them.
            if ( variant.OpacityState == Variant::Opacity::Baking && m_Frame > variant.BakeFrame )
                BuildWithMicromaps( mesh, masks, variant );
            return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( variant.Geometry.get() ) );
        }

        variant.Vertices = vertices;
        variant.Indices  = indices;
        variant.Binding  = binding;
        variant.Geometry.reset();
        // A new upload (or a dropped bake): the plain BLAS first; its segments are baked again when the metadata
        // names their alpha test.
        variant.OpacityState = Variant::Opacity::None;
        variant.Micromaps.clear();
        variant.BakedFrom.clear();
        // A row of its own (FPS-CPU-2): a variant built inside a frame's graph setup is CPU time the frame pays.
        DESERT_PROFILE_SCOPE( "RT: BLAS variant build" );
        const Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>> built = m_Build( mesh, masks, {} );
        if ( !built )
            return Common::MakeFormattedError<const RayTracingGeometryResource*>(
                 "the BLAS variant hiding submeshes {:#x}, non-opaque {:#x} at RT-LOD {} could not be built: {}",
                 masks.Hidden, masks.NonOpaque, masks.Lod, built.GetError() );
        variant.Geometry = built.GetValue();
        return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( variant.Geometry.get() ) );
    }

    void RayTracingGeometryVariants::EndFrame()
    {
        std::erase_if( m_Variants, []( const auto& entry ) { return !entry.second.Used; } );
        for ( auto& [key, variant] : m_Variants )
            variant.Used = false;
        ++m_Frame;
    }

    void RayTracingGeometryVariants::BuildWithMicromaps( Mesh& mesh, const RayTracingSubmeshMasks& masks,
                                                         Variant& variant )
    {
        DESERT_PROFILE_SCOPE( "RT: BLAS variant micromap build" );
        const Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>> built =
             m_Build( mesh, masks, variant.Micromaps );
        // The build's submission holds the bake outputs until it has read them (RayTracingSegmentOpacityMicromap::
        // Owner); the variant keeps only the counts.
        for ( RayTracingSegmentOpacityMicromap& micromap : variant.Micromaps )
            micromap.Owner.reset();
        if ( !built || !built.GetValue() )
        {
            variant.OpacityState = Variant::Opacity::Failed;
            m_OpacityErrors.push_back( std::format(
                 "the BLAS variant hiding submeshes {:#x}, non-opaque {:#x} at RT-LOD {} could not be rebuilt with "
                 "its opacity micromaps ({}); it keeps its BLAS without them",
                 masks.Hidden, masks.NonOpaque, masks.Lod, built ? "no BLAS" : built.GetError() ) );
            return;
        }
        variant.Geometry     = built.GetValue();
        variant.OpacityState = Variant::Opacity::Built;
    }

    std::vector<OpacityMicromapBake>
    RayTracingGeometryVariants::QueueOpacityMicromaps( std::span<const OpacityMicromapCandidate> candidates,
                                                       const AllocateBakeFn&                     allocate )
    {
        std::vector<OpacityMicromapBake> bakes;
        if ( candidates.empty() )
            return bakes;
        std::unordered_map<const RayTracingGeometryResource*, Variant*> byGeometry;
        for ( auto& [key, variant] : m_Variants )
            if ( variant.Geometry )
                byGeometry.emplace( variant.Geometry.get(), &variant );

        // CollectOpacityMicromapCandidates answers them grouped by BLAS, segments ascending.
        for ( std::size_t begin = 0; begin < candidates.size(); )
        {
            std::size_t end = begin + 1;
            while ( end < candidates.size() && candidates[end].Geometry == candidates[begin].Geometry )
                ++end;
            const std::span<const OpacityMicromapCandidate> group = candidates.subspan( begin, end - begin );
            begin                                                 = end;
            const auto found = byGeometry.find( group.front().Geometry );
            if ( found == byGeometry.end() )
                continue; // not a variant's BLAS
            Variant& variant = *found->second;
            if ( variant.OpacityState == Variant::Opacity::Failed )
                continue;
            if ( variant.OpacityState != Variant::Opacity::None )
            {
                const bool sameTest = std::equal(
                     group.begin(), group.end(), variant.BakedFrom.begin(), variant.BakedFrom.end(),
                     []( const OpacityMicromapCandidate& a, const OpacityMicromapCandidate& b )
                     { return a.Segment == b.Segment && a.Material == b.Material; } );
                if ( !sameTest )
                    variant.Vertices.reset(); // the next Resolve rebuilds it plain (and resets the state)
                continue;
            }

            std::vector<OpacityMicromapBake> own;
            own.reserve( group.size() );
            for ( const OpacityMicromapCandidate& candidate : group )
            {
                Common::ResultStr<RayTracingSegmentOpacityMicromap> output =
                     allocate( candidate.Segment, candidate.TriangleCount );
                if ( !output )
                {
                    m_OpacityErrors.push_back( std::format( "opacity micromap bake of segment {} ({} triangles): {}",
                                                            candidate.Segment, candidate.TriangleCount,
                                                            output.GetError() ) );
                    break;
                }
                own.push_back( { candidate, output.GetValue() } );
            }
            if ( own.size() != group.size() )
            {
                variant.OpacityState = Variant::Opacity::Failed;
                continue;
            }
            variant.OpacityState = Variant::Opacity::Baking;
            variant.BakeFrame    = m_Frame;
            variant.BakedFrom.assign( group.begin(), group.end() );
            variant.Micromaps.clear();
            for ( OpacityMicromapBake& bake : own )
            {
                variant.Micromaps.push_back( bake.Output );
                bakes.push_back( std::move( bake ) );
            }
        }
        return bakes;
    }

    RayTracingGeometryVariants::OpacityMicromapStats RayTracingGeometryVariants::GetOpacityMicromapStats() const
    {
        // data + VkMicromapTriangleEXT row + uint32 index per triangle (VulkanRayTracingGeometry
        // AllocateOpacityMicromapBake; 64 B of data at level 4).
        constexpr uint64_t kBytesPerTriangle = 64u + 8u + 4u;
        OpacityMicromapStats stats;
        for ( const auto& [key, variant] : m_Variants )
        {
            if ( variant.OpacityState == Variant::Opacity::Failed )
                ++stats.Failed;
            if ( variant.OpacityState != Variant::Opacity::Built )
                continue;
            for ( const RayTracingSegmentOpacityMicromap& micromap : variant.Micromaps )
            {
                ++stats.BakedSegments;
                stats.BakedTriangles += micromap.TriangleCount;
                stats.BakedBytes += uint64_t{ micromap.TriangleCount } * kBytesPerTriangle;
            }
        }
        return stats;
    }
} // namespace Desert::Graphic::RayTracing

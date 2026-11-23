#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometryVariants.hpp>

#include <Engine/Geometry/Mesh.hpp>

#include <Common/Core/Profiler.hpp>

namespace Desert::Graphic::RayTracing
{
    Common::ResultStr<const RayTracingGeometryResource*>
    RayTracingGeometryVariants::Resolve( Mesh& mesh, const RayTracingSubmeshMasks& masks )
    {
        const MeshGeometry& geometry = mesh.GetGeometry();
        if ( mesh.IsSkinned() || mesh.IsProceduralRayTracingGeometry() || !geometry.IsResident() ||
             geometry.IndexCount == 0u )
            return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( nullptr ) );

        const auto          vertices = geometry.Vertices;
        const auto          indices  = geometry.Indices;
        const auto          binding  = geometry.MeshBinding();
        const uint64_t      revision = mesh.GetRenderRevision();
        auto [found, inserted] = m_Variants.try_emplace( Key{ &mesh, masks } );
        Variant& variant       = found->second;
        variant.LastUsedFrame  = m_Frame;
        const bool sameUpload  = !inserted && variant.Vertices.lock().get() == vertices.get() &&
                                variant.Indices.lock().get() == indices.get() && variant.Binding == binding &&
                                variant.Revision == revision;
        if ( sameUpload )
            return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( variant.Geometry.get() ) );

        variant.Vertices = vertices;
        variant.Indices  = indices;
        variant.Binding  = binding;
        variant.Revision = revision;
        variant.Geometry.reset();
        // A row of its own (FPS-CPU-2): a BLAS built inside a frame's graph setup is CPU time the frame pays.
        DESERT_PROFILE_SCOPE( "RT: BLAS on-demand build" );
        const Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>> built = m_Build( mesh, masks );
        if ( !built )
            return Common::MakeFormattedError<const RayTracingGeometryResource*>(
                 "the BLAS hiding submeshes {:#x}, non-opaque {:#x} at RT-LOD {} could not be built: {}",
                 masks.Hidden, masks.NonOpaque, masks.Lod, built.GetError() );
        variant.Geometry = built.GetValue();
        return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( variant.Geometry.get() ) );
    }

    void RayTracingGeometryVariants::EndFrame()
    {
        const uint64_t frame = m_Frame++;
        std::erase_if( m_Variants, [frame]( const auto& entry )
                       { return frame - entry.second.LastUsedFrame >= kUnusedFramesBeforeRelease; } );
    }
} // namespace Desert::Graphic::RayTracing

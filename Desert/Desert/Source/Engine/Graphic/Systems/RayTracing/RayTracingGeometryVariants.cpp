#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometryVariants.hpp>

#include <Engine/Geometry/Mesh.hpp>

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
            return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( variant.Geometry.get() ) );

        variant.Vertices = vertices;
        variant.Indices  = indices;
        variant.Binding  = binding;
        variant.Geometry.reset();
        const Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>> built = m_Build( mesh, masks );
        if ( !built )
            return Common::MakeFormattedError<const RayTracingGeometryResource*>(
                 "the BLAS variant hiding submeshes {:#x}, non-opaque {:#x} could not be built: {}", masks.Hidden,
                 masks.NonOpaque, built.GetError() );
        variant.Geometry = built.GetValue();
        return Common::MakeSuccess( static_cast<const RayTracingGeometryResource*>( variant.Geometry.get() ) );
    }

    void RayTracingGeometryVariants::EndFrame()
    {
        std::erase_if( m_Variants, []( const auto& entry ) { return !entry.second.Used; } );
        for ( auto& [key, variant] : m_Variants )
            variant.Used = false;
    }
} // namespace Desert::Graphic::RayTracing

#include <Engine/Graphic/GlobalIllumination/GIHardwareTracer.hpp>

#include <format>

namespace Desert::Graphic::GlobalIllumination
{
    Common::ResultStr<HardwareTracerFrame>
    MakeHardwareTracerFrame( const Pipeline& pipeline, const RayTracing::RayTracingSceneFrame& scene,
                             const RayTracing::RayTracingMetadataFrame& metadata, RDG::BufferRef rowToCardRange )
    {
        constexpr std::string_view kFormat =
             "GI {}: {} (GIMethod::HardwareTraced needs it; the walk is not switched "
             "to the distance field)";
        if ( !pipeline.Traced || pipeline.Tracer != Tracer::HardwareRayQuery )
            return Common::MakeFormattedError<HardwareTracerFrame>(
                 "GI {}: the pipeline's tracer is not HardwareRayQuery; the software permutation reads no TLAS",
                 TracerDefine( Tracer::HardwareRayQuery ) );
        if ( !scene.Built || !scene.Tlas.IsValid() )
            return Common::MakeFormattedError<HardwareTracerFrame>(
                 kFormat, TracerDefine( Tracer::HardwareRayQuery ),
                 std::format( "no TLAS was built this frame ('{}' was not declared)",
                              RayTracing::kTlasPassName ) );
        if ( !metadata.Geometry.IsValid() || !metadata.Materials.IsValid() )
            return Common::MakeFormattedError<HardwareTracerFrame>(
                 kFormat, TracerDefine( Tracer::HardwareRayQuery ),
                 "the ray-tracing geometry / material records were not declared this frame" );
        if ( !rowToCardRange.IsValid() )
            return Common::MakeFormattedError<HardwareTracerFrame>(
                 kFormat, TracerDefine( Tracer::HardwareRayQuery ),
                 "the surface cache's instance-row table was not declared this frame" );

        return Common::MakeSuccess( HardwareTracerFrame{ .Tlas           = scene.Tlas,
                                                         .Geometry       = metadata.Geometry,
                                                         .Materials      = metadata.Materials,
                                                         .RowToCardRange = rowToCardRange } );
    }

    void DeclareHardwareTracerReads( RDG::BindingBlockBuilder& bindings, const HardwareTracerFrame& frame )
    {
        bindings.AccelerationStructure( kHardwareTracerTlasBinding, frame.Tlas )
             .Storage( kHardwareTracerGeometryBinding, frame.Geometry, RDG::Access::StorageRead )
             .Storage( kHardwareTracerMaterialsBinding, frame.Materials, RDG::Access::StorageRead )
             .Storage( kHardwareTracerRowCardsBinding, frame.RowToCardRange, RDG::Access::StorageRead );
    }
} // namespace Desert::Graphic::GlobalIllumination

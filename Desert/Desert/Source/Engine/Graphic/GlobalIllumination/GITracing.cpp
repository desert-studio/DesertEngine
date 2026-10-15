#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <format>

namespace Desert::Graphic::GlobalIllumination
{
    std::string_view TracerFrameMissing( const TracerFrame& frame )
    {
        switch ( frame.Pipeline.Tracer )
        {
            case Tracer::SoftwareDistanceField:
                for ( const RDG::TextureRef& level : frame.GlobalDistanceField )
                    if ( !level.IsValid() )
                        return "the global distance field was not brought up";
                return {};
            case Tracer::HardwareRayQuery:
                // Never answered by the distance field instead: a hardware pipeline without its TLAS is a defect
                // (ResolveRenderMethods lowered HardwareTraced on a device without ray query, and
                // RayTracingSceneRequired builds the TLAS for every frame whose GI traces hardware rays).
                if ( !frame.Tlas.IsValid() )
                    return "the view's TLAS was not built this frame";
                if ( !frame.Metadata.Geometry.IsValid() || !frame.Metadata.Materials.IsValid() )
                    return "the ray-tracing geometry / material records were not declared this frame";
                if ( !frame.InstanceMeshCards.IsValid() )
                    return "the surface cache's instance -> MeshCards table was not declared this frame "
                           "(GI-SURFACE-CACHE)";
                return {};
        }
        return "an unknown tracer";
    }

    void DeclareTracerReads( RDG::BindingBlockBuilder& block, const TracerFrame& frame )
    {
        switch ( frame.Pipeline.Tracer )
        {
            case Tracer::SoftwareDistanceField:
                for ( uint32_t level = 0; level < frame.GlobalDistanceField.size(); ++level )
                    block.Sampled( std::format( "u_GlobalDistanceField{}", level ),
                                   frame.GlobalDistanceField[level], RDG::Access::SampledCompute,
                                   RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() );
                return;
            case Tracer::HardwareRayQuery:
                block.AccelerationStructure( kHardwareTracerTlasName, frame.Tlas )
                     .Storage( kHardwareTracerGeometryName, frame.Metadata.Geometry, RDG::Access::StorageRead )
                     .Storage( kHardwareTracerMaterialsName, frame.Metadata.Materials, RDG::Access::StorageRead )
                     .Storage( kHardwareTracerInstanceMeshCardsName, frame.InstanceMeshCards,
                               RDG::Access::StorageRead );
                return;
        }
    }
} // namespace Desert::Graphic::GlobalIllumination

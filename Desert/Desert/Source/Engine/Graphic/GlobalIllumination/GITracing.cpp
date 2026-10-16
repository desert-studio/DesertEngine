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
                if ( !frame.GlobalDistanceFieldLevels.IsValid() )
                    return "the global distance field's clipmap placement was not uploaded";
                if ( !frame.GlobalDistanceFieldObjectGrid.IsValid() )
                    return "the global distance field's object grid was not brought up";
                return {};
            case Tracer::HardwareRayQuery:
                // Never answered by the distance field instead: a hardware pipeline without its TLAS is a defect
                // (ResolveRenderMethods lowered HardwareTraced on a device without ray query, and
                // RayTracingSceneRequired builds the TLAS for every frame whose GI traces hardware rays).
                if ( !frame.Tlas.IsValid() )
                    return "the view's TLAS was not built this frame";
                if ( !frame.Metadata.Geometry.IsValid() || !frame.Metadata.Materials.IsValid() )
                    return "the ray-tracing geometry / material records were not declared this frame";
                if ( !frame.InstanceSurfaceCacheRows.IsValid() )
                    return "the TLAS instance -> surface cache row table was not declared this frame "
                           "(GI-SURFACE-CACHE)";
                return {};
        }
        return "an unknown tracer";
    }

    void DeclareTracerReads( RDG::BindingBlockBuilder& block, const TracerFrame& frame, const RDG::Access sampled )
    {
        switch ( frame.Pipeline.Tracer )
        {
            case Tracer::SoftwareDistanceField:
                for ( uint32_t level = 0; level < frame.GlobalDistanceField.size(); ++level )
                    block.Texture( std::format( "u_GlobalDistanceField{}", level ),
                                   frame.GlobalDistanceField[level], sampled,
                                   RDG::SubresourceRange::All() )
                         .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() );
                block.Storage( "GITracerSoftwareUB", frame.GlobalDistanceFieldLevels, RDG::Access::StorageRead );
                // Fetched texel by texel through the includer's point-clamped shared state (GITracer.glslh).
                block.Texture( "u_GlobalDistanceFieldObjectGrid", frame.GlobalDistanceFieldObjectGrid, sampled,
                               RDG::SubresourceRange::All() )
                     .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() );
                return;
            case Tracer::HardwareRayQuery:
                block.AccelerationStructure( kHardwareTracerTlasName, frame.Tlas )
                     .Storage( kHardwareTracerGeometryName, frame.Metadata.Geometry, RDG::Access::StorageRead )
                     .Storage( kHardwareTracerMaterialsName, frame.Metadata.Materials, RDG::Access::StorageRead )
                     .Storage( kHardwareTracerInstanceSurfaceCacheRowsName, frame.InstanceSurfaceCacheRows,
                               RDG::Access::StorageRead );
                return;
        }
    }

    std::string_view SurfaceCacheFrameMissing( const SurfaceCacheFrame& frame )
    {
        if ( !frame.Cards.IsValid() || !frame.Pages.IsValid() || !frame.MeshCards.IsValid() )
            return "the surface cache's card records were not uploaded this frame";
        if ( !frame.InstanceRows.IsValid() )
            return "the surface cache's instance table was not uploaded this frame";
        if ( !frame.FinalLighting.IsValid() || !frame.Depth.IsValid() || !frame.Opacity.IsValid() )
            return "the surface cache's atlas layers were not imported this frame";
        return {};
    }

    void DeclareSurfaceCacheReads( RDG::BindingBlockBuilder& block, const SurfaceCacheFrame& frame )
    {
        const auto all = RDG::SubresourceRange::All();
        block.Storage( "SurfaceCardBuffer", frame.Cards, RDG::Access::StorageRead )
             .Storage( "SurfaceCardPageBuffer", frame.Pages, RDG::Access::StorageRead )
             .Storage( "SurfaceMeshCardsBuffer", frame.MeshCards, RDG::Access::StorageRead )
             .Storage( "SurfaceCacheInstancesUB", frame.InstanceRows, RDG::Access::StorageRead )
             .Texture( "u_SurfaceCacheFinalLighting", frame.FinalLighting, RDG::Access::SampledCompute, all )
             .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
             .Texture( "u_SurfaceCacheDepth", frame.Depth, RDG::Access::SampledCompute, all )
             .Texture( "u_SurfaceCacheOpacity", frame.Opacity, RDG::Access::SampledCompute, all );
    }
} // namespace Desert::Graphic::GlobalIllumination

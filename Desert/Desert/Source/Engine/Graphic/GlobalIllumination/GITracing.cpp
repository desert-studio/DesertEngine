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
                if ( !frame.MeshDistanceFields.IsValid() )
                    return "the mesh distance fields and their near-field grid were not brought up";
                return {};
            case Tracer::HardwareRayQuery:
                return "the ray-query tracer is not in this build (GI-HWRT)";
        }
        return "an unknown tracer";
    }

    void DeclareTracerReads( RDG::BindingBlockBuilder& block, const TracerFrame& frame, const RDG::Access sampled )
    {
        switch ( frame.Pipeline.Tracer )
        {
            case Tracer::SoftwareDistanceField:
                for ( uint32_t level = 0; level < frame.GlobalDistanceField.size(); ++level )
                    block.Sampled( std::format( "u_GlobalDistanceField{}", level ),
                                   frame.GlobalDistanceField[level], sampled, RDG::SubresourceRange::All(),
                                   RDG::SamplerDesc::LinearRepeat() );
                block.Storage( "GITracerSoftwareUB", frame.GlobalDistanceFieldLevels, RDG::Access::StorageRead )
                     .Storage( "GITracerDFAssetsUB", frame.MeshDistanceFields.Assets, RDG::Access::StorageRead )
                     .Storage( "GITracerDFIndirectionUB", frame.MeshDistanceFields.Indirection,
                               RDG::Access::StorageRead )
                     .Storage( "GITracerDFObjectsUB", frame.MeshDistanceFields.Objects, RDG::Access::StorageRead )
                     .Storage( "GITracerMeshSDFGridCellsUB", frame.MeshDistanceFields.NearFieldGrid,
                               RDG::Access::StorageRead )
                     .Sampled( "u_DFBrickAtlas", frame.MeshDistanceFields.BrickAtlas, sampled,
                               RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() );
                return;
            case Tracer::HardwareRayQuery:
                return; // GI-HWRT declares the TLAS here; TracerFrameMissing refuses the frame until then
        }
    }

    std::string_view SurfaceCacheFrameMissing( const SurfaceCacheFrame& frame )
    {
        if ( !frame.Cards.IsValid() || !frame.Pages.IsValid() || !frame.MeshCards.IsValid() )
            return "the surface cache's card records were not uploaded this frame";
        if ( !frame.InstanceRows.IsValid() || !frame.ObjectGrid.IsValid() )
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
             .Storage( "SurfaceCacheObjectGridUB", frame.ObjectGrid, RDG::Access::StorageRead )
             .Sampled( "u_SurfaceCacheFinalLighting", frame.FinalLighting, RDG::Access::SampledCompute, all,
                       RDG::SamplerDesc::PointClamp() )
             .Sampled( "u_SurfaceCacheDepth", frame.Depth, RDG::Access::SampledCompute, all,
                       RDG::SamplerDesc::PointClamp() )
             .Sampled( "u_SurfaceCacheOpacity", frame.Opacity, RDG::Access::SampledCompute, all,
                       RDG::SamplerDesc::PointClamp() );
    }
} // namespace Desert::Graphic::GlobalIllumination

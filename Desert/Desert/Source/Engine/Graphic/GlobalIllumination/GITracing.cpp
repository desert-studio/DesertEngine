#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>

#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <format>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct GITracingShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, GITracerSoftwareUB );
        DESERT_SHADER_PARAMETER( Texture, GlobalDistanceFieldObjectGridTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCardPageBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceMeshCardsBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, SurfaceCacheInstancesUB );
        DESERT_SHADER_PARAMETER( Texture, SurfaceCacheFinalLightingTexture );
        DESERT_SHADER_PARAMETER( Texture, SurfaceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, SurfaceCacheOpacityTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, GITracerDFAssetsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GITracerDFIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GITracerDFObjectsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GITracerMeshSDFGridCellsUB );
        DESERT_SHADER_PARAMETER( Texture, DistanceFieldBrickAtlasTexture );
    };
} // namespace

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
                if ( !frame.MeshDistanceFields.IsValid() )
                    return "the mesh distance fields and their near-field grid were not brought up";
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
                    block.Texture( kGlobalDistanceFieldClipmapTextures[level], frame.GlobalDistanceField[level],
                                   sampled, RDG::SubresourceRange::All() )
                         .Sampler( GITracingShaderParameters::LinearRepeatSampler,
                                   RDG::SamplerDesc::LinearRepeat() );
                block.Storage( GITracingShaderParameters::GITracerSoftwareUB, frame.GlobalDistanceFieldLevels,
                               RDG::Access::StorageRead )
                     .Storage( GITracingShaderParameters::GITracerDFAssetsUB, frame.MeshDistanceFields.Assets,
                               RDG::Access::StorageRead )
                     .Storage( GITracingShaderParameters::GITracerDFIndirectionUB,
                               frame.MeshDistanceFields.Indirection, RDG::Access::StorageRead )
                     .Storage( GITracingShaderParameters::GITracerDFObjectsUB, frame.MeshDistanceFields.Objects,
                               RDG::Access::StorageRead )
                     .Storage( GITracingShaderParameters::GITracerMeshSDFGridCellsUB,
                               frame.MeshDistanceFields.NearFieldGrid, RDG::Access::StorageRead )
                     // The brick atlas reads through the clipmaps' shared repeat state (GITracer.glslh).
                     .Texture( GITracingShaderParameters::DistanceFieldBrickAtlasTexture,
                               frame.MeshDistanceFields.BrickAtlas, sampled, RDG::SubresourceRange::All() )
                     .Sampler( GITracingShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                     // Fetched texel by texel through the includer's point-clamped shared state (GITracer.glslh).
                     .Texture( GITracingShaderParameters::GlobalDistanceFieldObjectGridTexture,
                               frame.GlobalDistanceFieldObjectGrid, sampled, RDG::SubresourceRange::All() )
                     .Sampler( GITracingShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() );
                return;
            case Tracer::HardwareRayQuery:
                block.AccelerationStructure( kHardwareTracerTlas, frame.Tlas )
                     .Storage( kHardwareTracerGeometry, frame.Metadata.Geometry, RDG::Access::StorageRead )
                     .Storage( kHardwareTracerMaterials, frame.Metadata.Materials, RDG::Access::StorageRead )
                     .Storage( kHardwareTracerInstanceSurfaceCacheRows, frame.InstanceSurfaceCacheRows,
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
        block.Storage( GITracingShaderParameters::SurfaceCardBuffer, frame.Cards, RDG::Access::StorageRead )
             .Storage( GITracingShaderParameters::SurfaceCardPageBuffer, frame.Pages, RDG::Access::StorageRead )
             .Storage( GITracingShaderParameters::SurfaceMeshCardsBuffer, frame.MeshCards,
                       RDG::Access::StorageRead )
             .Storage( GITracingShaderParameters::SurfaceCacheInstancesUB, frame.InstanceRows,
                       RDG::Access::StorageRead )
             .Texture( GITracingShaderParameters::SurfaceCacheFinalLightingTexture, frame.FinalLighting,
                       RDG::Access::SampledCompute, all )
             .Sampler( GITracingShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Texture( GITracingShaderParameters::SurfaceCacheDepthTexture, frame.Depth,
                       RDG::Access::SampledCompute, all )
             .Texture( GITracingShaderParameters::SurfaceCacheOpacityTexture, frame.Opacity,
                       RDG::Access::SampledCompute, all );
    }
} // namespace Desert::Graphic::GlobalIllumination

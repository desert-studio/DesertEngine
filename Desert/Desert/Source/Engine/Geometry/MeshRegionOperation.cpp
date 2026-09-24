#include "MeshRegionOperation.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshTangents.hpp"
#include "Engine/Geometry/UECore/Operations/InsetMeshRegion.hpp"
#include "Engine/Geometry/UECore/Operations/OffsetMeshRegion.hpp"

namespace Desert::Geometry
{
    const char* ToString( RegionOperation operation )
    {
        switch ( operation )
        {
            case RegionOperation::Extrude:
                return "Extrude";
            case RegionOperation::PushPull:
                return "Push/Pull";
            case RegionOperation::Inset:
                return "Inset";
            case RegionOperation::Outset:
                return "Outset";
        }
        return "Unknown";
    }

    namespace
    {
        // UE rebuilds tangents after every edit (they are derived data); the per-triangle path is exact on the
        // flat faces the region operations create.
        Common::BoolResultStr RecomputeTangents( FDynamicMesh3& mesh, const char* name )
        {
            FDynamicMeshAttributeSet* attributes = mesh.Attributes();
            if ( attributes == nullptr || !attributes->HasTangentSpace() )
                return Common::MakeSuccess( true );
            const FDynamicMeshUVOverlay* uvs = attributes->PrimaryUV();
            if ( uvs == nullptr )
                return Common::MakeFormattedError<bool>(
                     "Mesh {}: the mesh carries tangents but has no UV layer 0 to derive them from", name );
            FMeshTangentsd tangents( &mesh );
            tangents.ComputeSeparatePerTriangleTangents( attributes->PrimaryNormals(), uvs );
            if ( !tangents.CopyToOverlays( mesh ) )
                return Common::MakeFormattedError<bool>(
                     "Mesh {}: tangents could not be written - {} normal layers, 3 expected", name,
                     attributes->NumNormalLayers() );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<RegionOutcome> RunRegionOperation( RegionOperation operation, const FDynamicMesh3& before,
                                                         const ElementSelection& selection, float distance )
    {
        const char* name = ToString( operation );
        if ( distance == 0.0f )
            return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: distance 0 cm changes nothing", name );
        if ( distance < 0.0f && operation != RegionOperation::PushPull )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh {}: distance {} cm is negative - {}", name, distance,
                 operation == RegionOperation::Extrude ? "Push/Pull takes a signed distance"
                 : operation == RegionOperation::Inset ? "use Outset"
                                                       : "use Inset" );
        const FGroupTopology   topology( &before, true );
        const ElementSelection triangles = ConvertSelection( before, topology, selection, ElementMode::Triangle );
        if ( triangles.Empty() )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh {}: the {} selected {} cover no whole triangle", name, selection.Size(),
                 ToString( selection.Mode() ) );
        TArray<int32> regionTriangles;
        for ( const int t : triangles.Ids() )
            regionTriangles.Add( t );

        auto          mesh = std::make_shared<FDynamicMesh3>( before );
        TArray<int32> resultTriangles;
        if ( operation == RegionOperation::Extrude || operation == RegionOperation::PushPull )
        {
            FOffsetMeshRegion extruder( mesh.get() );
            extruder.Triangles = regionTriangles;
            // UE's Extrude default (SelectedTriangleNormalsEven): every selected face moves by the full
            // distance. Push/Pull is FExtrudeOp's SingleDirection: one direction, the region's area-weighted
            // normal, so the walls are parallel and a push into a solid cannot fold them.
            extruder.ExtrusionVectorType =
                 FOffsetMeshRegion::EVertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAdjusted;
            extruder.DefaultOffsetDistance = distance;
            if ( operation == RegionOperation::PushPull )
            {
                FVector3d direction( 0.0, 0.0, 0.0 );
                for ( const int t : triangles.Ids() )
                    direction += before.GetTriNormal( t ) * before.GetTriArea( t );
                if ( direction.Length() <= 0.0 )
                    return Common::MakeFormattedError<RegionOutcome>(
                         "Mesh {}: the {} selected triangles have no average normal (they cancel out)", name,
                         triangles.Size() );
                Normalize( direction );
                const double signedDistance = distance;
                extruder.OffsetPositionFunc =
                     [direction, signedDistance]( const FVector3d& position, const FVector3d&, int )
                { return position + direction * signedDistance; };
            }
            extruder.bIsPositiveOffset = distance > 0.0f;
            if ( !extruder.Apply() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: {}", name, extruder.FailureReason );
            for ( const auto& region : extruder.OffsetRegions )
                for ( const int32 t : region.OffsetTids )
                    resultTriangles.Add( t );
        }
        else
        {
            FInsetMeshRegion inset( mesh.get() );
            inset.Triangles     = regionTriangles;
            inset.InsetDistance = operation == RegionOperation::Outset ? -distance : distance;
            if ( !inset.Apply() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: {}", name, inset.FailureReason );
            for ( const auto& region : inset.InsetRegions )
                for ( const int32 t : region.InitialTriangles )
                    resultTriangles.Add( t );
        }
        if ( auto tangents = RecomputeTangents( *mesh, name ); !tangents.IsSuccess() )
            return Common::MakeError<RegionOutcome>( tangents.GetError() );

        ElementSelection result( ElementMode::Triangle );
        for ( const int32 t : resultTriangles )
            if ( auto added = result.Add( *mesh, t ); !added.IsSuccess() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: result triangle {}: {}", name, t,
                                                                  added.GetError() );
        if ( selection.Mode() != ElementMode::Triangle )
        {
            const FGroupTopology afterTopology( mesh.get(), true );
            result = ConvertSelection( *mesh, afterTopology, result, selection.Mode() );
        }
        return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), std::move( result ) } );
    }
} // namespace Desert::Geometry

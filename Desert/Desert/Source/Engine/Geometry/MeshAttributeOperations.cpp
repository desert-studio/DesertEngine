// EditNormals: ported from UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingOperators/Private/
// CleaningOps/EditNormalsOp.cpp:47-150 (FEditNormalsOp::CalculateResultWholeMesh), adapted: UE Core as std/glm,
// result as ResultStr. Not yet ported: bFixInconsistentNormals (needs FMeshRepairOrientation).

#include "Engine/Geometry/MeshAttributeOperations.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/Polygroups/PolygroupSet.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <spdlog/fmt/fmt.h>

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

namespace Desert::Geometry
{
    const char* ToString( NormalCalculationMethod method )
    {
        switch ( method )
        {
            case NormalCalculationMethod::AreaWeighted:
                return "Area Weighted";
            case NormalCalculationMethod::AngleWeighted:
                return "Angle Weighted";
            case NormalCalculationMethod::AreaAngleWeighting:
                return "Area Angle Weighting";
        }
        return "?";
    }

    const char* ToString( SplitNormalMethod method )
    {
        switch ( method )
        {
            case SplitNormalMethod::UseExistingTopology:
                return "Use Existing Topology";
            case SplitNormalMethod::FaceNormalThreshold:
                return "Face Normal Threshold";
            case SplitNormalMethod::FaceGroupID:
                return "Face Group ID";
            case SplitNormalMethod::PerTriangle:
                return "Per Triangle";
            case SplitNormalMethod::PerVertex:
                return "Per Vertex";
        }
        return "?";
    }

    Common::ResultStr<DeformOutcome> EditNormals( const DynamicMesh3& input, const NormalsSettings& settings )
    {
        if ( input.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "Normals: the mesh has no triangle" );
        if ( !( settings.SharpEdgeAngleThreshold >= 0.0f && settings.SharpEdgeAngleThreshold <= 180.0f ) )
            return Common::MakeError<DeformOutcome>(
                 fmt::format( "Normals: Sharp Edge Angle Threshold {} is outside 0..180 degrees",
                              settings.SharpEdgeAngleThreshold ) );

        DynamicMesh3 mesh = input;
        if ( !mesh.HasAttributes() )
            mesh.EnableAttributes();
        DynamicMeshNormalOverlay* normals = mesh.Attributes()->PrimaryNormals();
        if ( normals == nullptr )
            return Common::MakeError<DeformOutcome>( "Normals: the mesh has no primary normal overlay" );

        // Splitting always recomputes as well.
        const bool needsRecompute =
             settings.RecomputeNormals || settings.SplitMethod != SplitNormalMethod::UseExistingTopology;

        if ( settings.InvertNormals )
        {
            for ( const int tid : mesh.TriangleIndicesItr() )
                mesh.ReverseTriOrientation( tid );
            // Also reverse the normal directions, unless the recompute below derives them from the new winding.
            if ( !needsRecompute )
                for ( const int eid : normals->ElementIndicesItr() )
                    normals->SetElement( eid, -normals->GetElement( eid ) );
        }

        const double dotThreshold =
             std::cos( glm::radians( static_cast<double>( settings.SharpEdgeAngleThreshold ) ) );
        MeshNormals faceNormals( &mesh );
        switch ( settings.SplitMethod )
        {
            case SplitNormalMethod::UseExistingTopology:
                break;
            case SplitNormalMethod::FaceNormalThreshold:
            {
                faceNormals.ComputeTriangleNormals();
                const std::vector<glm::dvec3>& n = faceNormals.GetNormals();
                normals->CreateFromPredicate( [&n, dotThreshold]( int, int ta, int tb )
                                              { return glm::dot( n[ta], n[tb] ) > dotThreshold; }, 0.0f );
                break;
            }
            case SplitNormalMethod::PerTriangle:
                MeshNormals::InitializeMeshToPerTriangleNormals( &mesh );
                break;
            case SplitNormalMethod::PerVertex:
                MeshNormals::InitializeOverlayToPerVertexNormals( normals, false );
                break;
            case SplitNormalMethod::FaceGroupID:
            {
                const PolygroupSet groups( &mesh );
                normals->CreateFromPredicate(
                     [&groups]( int, int ta, int tb )
                     { return groups.GetTriangleGroup( ta ) == groups.GetTriangleGroup( tb ); }, 0.0f );
                break;
            }
        }

        const bool areaWeight = settings.CalculationMethod == NormalCalculationMethod::AreaWeighted ||
                                settings.CalculationMethod == NormalCalculationMethod::AreaAngleWeighting;
        const bool angleWeight = settings.CalculationMethod == NormalCalculationMethod::AngleWeighted ||
                                 settings.CalculationMethod == NormalCalculationMethod::AreaAngleWeighting;
        if ( needsRecompute )
        {
            MeshNormals recomputed( &mesh );
            recomputed.RecomputeOverlayNormals( normals, areaWeight, angleWeight );
            recomputed.CopyToOverlay( normals, false );
        }

        if ( settings.SplitMethod == SplitNormalMethod::FaceNormalThreshold && settings.AllowSharpVertices )
        {
            const std::vector<glm::dvec3>& n = faceNormals.GetNormals();
            normals->SplitVerticesWithPredicate(
                 [normals, &n, dotThreshold]( int eid, int tid )
                 { return glm::dot( glm::dvec3( normals->GetElement( eid ) ), n[tid] ) <= dotThreshold; },
                 [&n]( int, int tid, float* fill )
                 {
                     fill[0] = static_cast<float>( n[tid].x );
                     fill[1] = static_cast<float>( n[tid].y );
                     fill[2] = static_cast<float>( n[tid].z );
                 } );
        }

        if ( mesh.HasVertexNormals() )
            MeshNormals::QuickComputeVertexNormals( mesh );
        if ( auto tangents = RecomputeTangentSpace( mesh, "Normals" ); !tangents.IsSuccess() )
            return Common::MakeError<DeformOutcome>( tangents.GetError() );

        DeformOutcome out;
        out.Report = fmt::format(
             "{} normal elements over {} vertices; split {}, {}{}{}", normals->ElementCount(), mesh.VertexCount(),
             ToString( settings.SplitMethod ), ToString( settings.CalculationMethod ),
             settings.InvertNormals ? ", inverted" : "", needsRecompute ? "" : ", values kept" );
        out.Mesh = std::move( mesh );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry

// EditNormals: ported from UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingOperators/Private/
// CleaningOps/EditNormalsOp.cpp:47-150 (FEditNormalsOp::CalculateResultWholeMesh), adapted: UE Core as std/glm,
// result as ResultStr. RepairOrientation: ported from UE 5.8
// GeometryProcessing/Source/DynamicMesh/Private/Operations/ RepairOrientation.cpp
// (FMeshRepairOrientation::OrientComponents, ComputeStatistics, SolveGlobalOrientation).

#include "Engine/Geometry/MeshAttributeOperations.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshTangents.hpp"

#include <mikktspace.h>
#include "Engine/Geometry/MeshCore/IndexUtil.hpp"
#include "Engine/Geometry/MeshCore/Polygroups/PolygroupSet.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"
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

    int RepairOrientation( DynamicMesh3& mesh )
    {
        // OrientComponents: flood each connected component from a seed triangle; a neighbour that walks the
        // shared edge in the same direction as the current triangle has the opposite winding and is reversed.
        struct Component
        {
            std::vector<int> Triangles;
            double           OutFacing = 0.0;
            double           InFacing  = 0.0;
        };
        std::vector<Component> components;
        std::vector<int>       componentOf( static_cast<size_t>( mesh.MaxTriangleID() ), -1 );
        std::vector<int>       stack;
        int                    reversed = 0;
        for ( const int seed : mesh.TriangleIndicesItr() )
        {
            if ( componentOf[seed] >= 0 )
                continue;
            const int  index  = static_cast<int>( components.size() );
            Component& c      = components.emplace_back();
            componentOf[seed] = index;
            stack.push_back( seed );
            while ( !stack.empty() )
            {
                const int tid = stack.back();
                stack.pop_back();
                c.Triangles.push_back( tid );
                const Index3i tri   = mesh.GetTriangle( tid );
                const Index3i edges = mesh.GetTriEdges( tid );
                for ( int j = 0; j < 3; ++j )
                {
                    const Index2i edgeT = mesh.GetEdgeT( edges[j] );
                    const int     nbr   = edgeT.A == tid ? edgeT.B : edgeT.A;
                    if ( nbr == DynamicMesh3::InvalidID || componentOf[nbr] >= 0 )
                        continue;
                    const Index2i edgeV = mesh.GetEdgeV( edges[j] );
                    // the edge as the current triangle walks it
                    int a = edgeV.A, b = edgeV.B;
                    if ( IndexUtil::FindTriOrderedEdge( a, b, tri ) == IndexConstants::InvalidID )
                        std::swap( a, b );
                    if ( IndexUtil::FindTriOrderedEdge( a, b, mesh.GetTriangle( nbr ) ) !=
                         IndexConstants::InvalidID )
                    {
                        mesh.ReverseTriOrientation( nbr );
                        ++reversed;
                    }
                    componentOf[nbr] = index;
                    stack.push_back( nbr );
                }
            }
        }

        // ComputeStatistics: from a far point on each side of a triangle, ray-cast back at its component. A
        // triangle hit from the front side is evidence the component faces out; from the back side, in.
        const DynamicMeshAABBTree3 tree( &mesh );
        const double               dist = 2.0 * mesh.GetBounds().DiagonalLength();
        for ( int ci = 0; ci < static_cast<int>( components.size() ); ++ci )
        {
            Component&                               c = components[ci];
            const DynamicMeshAABBTree3::QueryOptions options( [&componentOf, ci]( int tid )
                                                              { return componentOf[tid] == ci; } );
            for ( const int tid : c.Triangles )
            {
                glm::dvec3 normal, centroid;
                double     area = 0.0;
                mesh.GetTriInfo( tid, normal, area, centroid );
                if ( area < 1e-12 )
                    continue;
                double t = 0.0;
                if ( tree.FindNearestHitTriangle( centroid + dist * normal, -normal, t, options ) == tid )
                    c.OutFacing += area;
                if ( tree.FindNearestHitTriangle( centroid - dist * normal, normal, t, options ) == tid )
                    c.InFacing += area;
            }
        }

        // SolveGlobalOrientation
        for ( const Component& c : components )
            if ( c.InFacing > c.OutFacing )
                for ( const int tid : c.Triangles )
                {
                    mesh.ReverseTriOrientation( tid );
                    ++reversed;
                }
        return reversed;
    }

    const char* ToString( TangentsMethod method )
    {
        switch ( method )
        {
            case TangentsMethod::FastMikkTSpace:
                return "Fast MikkT Space";
            case TangentsMethod::MikkTSpace:
                return "MikkT Space";
            case TangentsMethod::PerTriangle:
                return "Per Triangle";
        }
        return "?";
    }

    namespace
    {
        // UE GeometryAlgorithms DynamicMeshMikkTWrapper.cpp (DynamicMeshMikkTInterface + ComputeTangents).
        struct MikkTMeshInfo
        {
            const DynamicMesh3*             Mesh;
            const DynamicMeshNormalOverlay* NormalOverlay;
            const DynamicMeshUVOverlay*     UVOverlay;
            MeshTangentsd*                  TangentsOut;
        };
        const MikkTMeshInfo& InfoOf( const SMikkTSpaceContext* c )
        {
            return *static_cast<const MikkTMeshInfo*>( c->m_pUserData );
        }
        int MikkGetNumFaces( const SMikkTSpaceContext* c )
        {
            return InfoOf( c ).Mesh->MaxTriangleID();
        }
        int MikkGetNumVertsOfFace( const SMikkTSpaceContext* c, const int f )
        {
            return InfoOf( c ).Mesh->IsTriangle( f ) ? 3 : 0;
        }
        void MikkGetPosition( const SMikkTSpaceContext* c, float p[3], const int f, const int v )
        {
            const glm::dvec3 pos = InfoOf( c ).Mesh->GetVertex( InfoOf( c ).Mesh->GetTriangle( f )[v] );
            p[0] = float( pos.x ), p[1] = float( pos.y ), p[2] = float( pos.z );
        }
        glm::vec3 MikkNormal( const MikkTMeshInfo& m, int f, int v )
        {
            return m.NormalOverlay->IsSetTriangle( f ) ? m.NormalOverlay->GetElement( m.NormalOverlay->GetTriangle( f )[v] )
                                                       : glm::vec3( 0.0f, 0.0f, 1.0f );
        }
        void MikkGetNormal( const SMikkTSpaceContext* c, float n[3], const int f, const int v )
        {
            const glm::vec3 nn = MikkNormal( InfoOf( c ), f, v );
            n[0] = nn.x, n[1] = nn.y, n[2] = nn.z;
        }
        void MikkGetTexCoord( const SMikkTSpaceContext* c, float uv[2], const int f, const int v )
        {
            const MikkTMeshInfo& m = InfoOf( c );
            if ( !m.UVOverlay->IsSetTriangle( f ) )
            {
                uv[0] = uv[1] = 0.0f;
                return;
            }
            const glm::vec2 e = m.UVOverlay->GetElement( m.UVOverlay->GetTriangle( f )[v] );
            uv[0] = e.x, uv[1] = e.y;
        }
        void MikkSetTSpaceBasic( const SMikkTSpaceContext* c, const float t[3], const float sign, const int f,
                                 const int v )
        {
            const MikkTMeshInfo& m = InfoOf( c );
            const glm::dvec3     tangent( t[0], t[1], t[2] );
            const glm::dvec3     normal( MikkNormal( m, f, v ) );
            // UE VectorUtil::Bitangent( Normal, Tangent, -BitangentSign )
            m.TangentsOut->SetPerTriangleTangent( f, v, tangent, -double( sign ) * glm::cross( normal, tangent ) );
        }
        void ComputeMikkTTangents( const DynamicMesh3& mesh, MeshTangentsd& tangents )
        {
            tangents.InitializeTriVertexTangents( true );
            MikkTMeshInfo info{ &mesh, mesh.Attributes()->PrimaryNormals(), mesh.Attributes()->GetUVLayer( 0 ),
                                &tangents };
            SMikkTSpaceInterface iface{};
            iface.m_getNumFaces          = MikkGetNumFaces;
            iface.m_getNumVerticesOfFace = MikkGetNumVertsOfFace;
            iface.m_getPosition          = MikkGetPosition;
            iface.m_getNormal            = MikkGetNormal;
            iface.m_getTexCoord          = MikkGetTexCoord;
            iface.m_setTSpaceBasic       = MikkSetTSpaceBasic;
            iface.m_setTSpace            = nullptr;
            SMikkTSpaceContext context{};
            context.m_pInterface = &iface;
            context.m_pUserData  = &info;
            genTangSpaceDefault( &context );
        }
    } // namespace

    Common::ResultStr<DeformOutcome> ComputeTangents( const DynamicMesh3& input, const TangentsSettings& settings )
    {
        if ( input.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "Tangents: the mesh has no triangle" );
        DynamicMesh3 mesh = input;
        if ( !mesh.HasAttributes() )
            mesh.EnableAttributes();
        DynamicMeshAttributeSet* attributes = mesh.Attributes();
        // UE FCalculateTangentsOp bNoAttributesError.
        if ( attributes->NumUVLayers() < 1 || attributes->PrimaryNormals() == nullptr )
            return Common::MakeError<DeformOutcome>( "Tangents: the mesh has no UV layer or no normals" );
        if ( !attributes->HasTangentSpace() )
            attributes->EnableTangents();
        MeshTangentsd tangents( &mesh );
        switch ( settings.CalculationMethod )
        {
            case TangentsMethod::FastMikkTSpace:
                tangents.ComputeTriVertexTangents( attributes->PrimaryNormals(), attributes->GetUVLayer( 0 ),
                                                   { .bAngleWeighted = true, .bAveraged = true } );
                break;
            case TangentsMethod::PerTriangle:
                tangents.ComputeTriVertexTangents( attributes->PrimaryNormals(), attributes->GetUVLayer( 0 ),
                                                   { .bAngleWeighted = true, .bAveraged = false } );
                break;
            case TangentsMethod::MikkTSpace:
                ComputeMikkTTangents( mesh, tangents );
                break;
        }
        if ( !tangents.CopyToOverlays( mesh ) )
            return Common::MakeError<DeformOutcome>( "Tangents: could not write the tangent overlays" );
        DeformOutcome out;
        out.Report = fmt::format( "Tangents ({}): {} triangles, {} degenerate (zero UV area)",
                                  ToString( settings.CalculationMethod ), mesh.TriangleCount(),
                                  tangents.GetDegenerateTris().size() );
        out.Mesh   = std::move( mesh );
        return Common::MakeSuccess( std::move( out ) );
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

        // Splitting always recomputes as well; so does a winding repair (the kept normals would point the old
        // way).
        const bool needsRecompute = settings.RecomputeNormals || settings.FixInconsistentNormals ||
                                    settings.SplitMethod != SplitNormalMethod::UseExistingTopology;

        int repaired = 0;
        if ( settings.FixInconsistentNormals )
            repaired = RepairOrientation( mesh );

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
             "{} normal elements over {} vertices; split {}, {}{}{}{}", normals->ElementCount(),
             mesh.VertexCount(), ToString( settings.SplitMethod ), ToString( settings.CalculationMethod ),
             settings.FixInconsistentNormals ? fmt::format( ", {} triangles reoriented", repaired )
                                             : std::string(),
             settings.InvertNormals ? ", inverted" : "", needsRecompute ? "" : ", values kept" );
        out.Mesh = std::move( mesh );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry

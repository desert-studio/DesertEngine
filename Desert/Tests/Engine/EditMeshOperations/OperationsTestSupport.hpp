#pragma once

// Shared by the operation suites: cube / cylinder fixtures from the M5 generators and the closedness,
// volume and polygroup measures every operation is checked against.

#include <gtest/gtest.h>

#include <EditMeshTestSupport.hpp>

#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshNormals.hpp>
#include <Engine/Geometry/EditMeshOperations.hpp>
#include <Engine/Geometry/EditMeshPolyGroups.hpp>
#include <Engine/Geometry/EditMeshTopologyOperations.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace OperationsTest
{
    using namespace Desert::Geometry;
    using EditMeshTest::Valid;

    inline EditMesh Import( const ShapeMesh& shape )
    {
        RenderMeshData render;
        render.Vertices = shape.Vertices;
        render.Indices  = shape.Indices;
        auto imported   = FromRenderMesh( render );
        EXPECT_TRUE( imported.IsSuccess() ) << imported.GetError();
        EditMesh mesh = std::move( imported.GetValue().Mesh );
        return mesh;
    }

    inline double SignedVolume( const EditMesh& mesh )
    {
        double volume = 0.0;
        for ( const int t : mesh.TriangleIds() )
        {
            const auto& c = mesh.GetTriangle( t );
            volume += glm::dot( mesh.GetPosition( c[0] ),
                                glm::cross( mesh.GetPosition( c[1] ), mesh.GetPosition( c[2] ) ) ) /
                      6.0;
        }
        return volume;
    }

    inline double Area( const EditMesh& mesh, const std::vector<int>& triangles )
    {
        double area = 0.0;
        for ( const int t : triangles )
        {
            const auto& c = mesh.GetTriangle( t );
            area += 0.5 * glm::length( glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                                                   mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) ) );
        }
        return area;
    }

    // 200 cm box, base at y = 0: x, z in [-100, 100], y in [0, 200]; six polygroups, wound outwards.
    inline EditMesh MakeCube()
    {
        EditMesh mesh = Import( MakeBox( glm::vec3( 200.0f ) ) );
        EXPECT_EQ( GeneratePolyGroupsByAngle( mesh, 30.0f ), 6 );
        EXPECT_NEAR( SignedVolume( mesh ), 8.0e6, 1.0 );
        return mesh;
    }

    // 12 slices, diameter 200, height 200: wall + two caps = three polygroups, wound outwards (the signed
    // volume is the 12-gon prism's, 30000 cm^2 x 200 cm).
    inline EditMesh MakeCylinder12()
    {
        EditMesh mesh = Import( MakeCylinder( 200.0f, 200.0f, 12 ) );
        EXPECT_EQ( GeneratePolyGroupsByAngle( mesh, 45.0f ), 3 );
        EXPECT_NEAR( SignedVolume( mesh ), 6.0e6, 1.0 );
        return mesh;
    }

    // The polygroup whose triangles face `direction`.
    inline int GroupFacing( const EditMesh& mesh, const glm::vec3& direction )
    {
        for ( const int t : mesh.TriangleIds() )
            if ( glm::dot( TriangleNormal( mesh, t ), direction ) > 0.99f )
                return mesh.Attributes().GetPolyGroup( t );
        ADD_FAILURE() << "no triangle faces the direction";
        return InvalidId;
    }

    inline ElementSelection Select( const EditMesh& mesh, ElementMode mode, std::initializer_list<int> ids )
    {
        ElementSelection selection( mode );
        for ( const int id : ids )
            EXPECT_TRUE( selection.Add( mesh, id ).IsSuccess() );
        return selection;
    }

    inline std::vector<int> TrianglesOf( const EditMesh& mesh, const ElementSelection& selection )
    {
        const ElementSelection triangles = ConvertSelection( mesh, selection, ElementMode::Triangle );
        return { triangles.Ids().begin(), triangles.Ids().end() };
    }

    inline std::set<std::pair<int, int>> OpenEdges( const EditMesh& mesh )
    {
        std::set<std::pair<int, int>> open;
        for ( const int e : mesh.EdgeIds() )
            if ( mesh.IsBoundaryEdge( e ) )
                open.insert( { mesh.GetEdgeVertices( e )[0], mesh.GetEdgeVertices( e )[1] } );
        return open;
    }

    // The region's boundary edges (vertex pairs) on the mesh it was selected on.
    inline std::set<std::pair<int, int>> RegionBoundary( const EditMesh& mesh, const std::vector<int>& triangles )
    {
        const std::set<int>           in( triangles.begin(), triangles.end() );
        std::set<std::pair<int, int>> boundary;
        for ( const int t : triangles )
            for ( const int e : mesh.GetTriangleEdges( t ) )
            {
                const auto& et    = mesh.GetEdgeTriangles( e );
                const int   other = et[0] == t ? et[1] : et[0];
                if ( other == InvalidId || !in.count( other ) )
                    boundary.insert( { mesh.GetEdgeVertices( e )[0], mesh.GetEdgeVertices( e )[1] } );
            }
        return boundary;
    }

    inline int GroupCount( const EditMesh& mesh )
    {
        std::set<int> groups;
        for ( const int t : mesh.TriangleIds() )
            groups.insert( mesh.Attributes().GetPolyGroup( t ) );
        return static_cast<int>( groups.size() );
    }

    // Closed, manifold, valid, and every corner's normal on the side the winding faces: with a positive
    // volume that is "outwards".
    inline ::testing::AssertionResult ClosedAndConsistent( const EditMesh& mesh )
    {
        if ( auto valid = Valid( mesh ); !valid )
            return valid;
        if ( !mesh.IsManifold() )
            return ::testing::AssertionFailure() << "a bowtie vertex";
        if ( const auto open = OpenEdges( mesh ); !open.empty() )
            return ::testing::AssertionFailure() << open.size() << " open edges, first (" << open.begin()->first
                                                 << ", " << open.begin()->second << ")";
        const NormalOverlay* normals = mesh.Attributes().Normals();
        for ( const int t : mesh.TriangleIds() )
        {
            if ( !normals->IsSetTriangle( t ) )
                return ::testing::AssertionFailure() << "triangle " << t << " has no normals";
            for ( const int e : normals->GetTriangle( t ) )
                if ( glm::dot( normals->GetElement( e ), TriangleNormal( mesh, t ) ) <= 0.0f )
                    return ::testing::AssertionFailure()
                           << "triangle " << t << " has a normal against its winding";
        }
        if ( auto render = ToRenderMesh( mesh ); !render.IsSuccess() )
            return ::testing::AssertionFailure() << "not renderable: " << render.GetError();
        return ::testing::AssertionSuccess();
    }

    inline float Top( const EditMesh& mesh, const ElementSelection& selection )
    {
        float y = -1e9f;
        for ( const int t : TrianglesOf( mesh, selection ) )
            for ( const int v : mesh.GetTriangle( t ) )
                y = std::max( y, mesh.GetPosition( v ).y );
        return y;
    }
} // namespace OperationsTest

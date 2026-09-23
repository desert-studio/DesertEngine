#pragma once

// Builders and the validity matcher shared by the EditMesh suite's files: one copy, so a fixture the two
// files both rely on cannot drift between them.

#include <gtest/gtest.h>

#include <Engine/Geometry/EditMesh.hpp>

namespace EditMeshTest
{
    using Desert::Geometry::EditMesh;
    using Desert::Geometry::EditResult;
    using Desert::Geometry::InvalidId;

    // gtest prints the invariant CheckValidity names, so a red run says WHICH rule and which IDs.
    inline ::testing::AssertionResult Valid( const EditMesh& mesh )
    {
        const auto result = mesh.CheckValidity();
        if ( result.IsSuccess() )
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << result.GetError();
    }

    inline int Tri( EditMesh& mesh, int a, int b, int c )
    {
        int        t = InvalidId;
        const auto r = mesh.AppendTriangle( a, b, c, t );
        EXPECT_EQ( r, EditResult::Ok ) << ToString( r ) << " for (" << a << ", " << b << ", " << c << ")";
        return t;
    }

    // n x n quads in the z = 0 plane, every triangle wound counter-clockwise seen from +z.
    inline EditMesh MakeGrid( int n, float cell = 100.0f )
    {
        EditMesh mesh;
        for ( int y = 0; y <= n; ++y )
            for ( int x = 0; x <= n; ++x )
                mesh.AppendVertex( { x * cell, y * cell, 0.0f } );
        const int stride = n + 1;
        for ( int y = 0; y < n; ++y )
            for ( int x = 0; x < n; ++x )
            {
                const int a = y * stride + x;
                const int b = a + 1;
                const int c = a + stride;
                const int d = c + 1;
                Tri( mesh, a, b, d );
                Tri( mesh, a, d, c );
            }
        return mesh;
    }

    // A closed octahedron: every edge interior, every vertex manifold.
    inline EditMesh MakeOctahedron()
    {
        EditMesh  mesh;
        const int px = mesh.AppendVertex( { 100, 0, 0 } );
        const int nx = mesh.AppendVertex( { -100, 0, 0 } );
        const int py = mesh.AppendVertex( { 0, 100, 0 } );
        const int ny = mesh.AppendVertex( { 0, -100, 0 } );
        const int pz = mesh.AppendVertex( { 0, 0, 100 } );
        const int nz = mesh.AppendVertex( { 0, 0, -100 } );
        Tri( mesh, px, py, pz );
        Tri( mesh, py, nx, pz );
        Tri( mesh, nx, ny, pz );
        Tri( mesh, ny, px, pz );
        Tri( mesh, py, px, nz );
        Tri( mesh, nx, py, nz );
        Tri( mesh, ny, nx, nz );
        Tri( mesh, px, ny, nz );
        return mesh;
    }

} // namespace EditMeshTest

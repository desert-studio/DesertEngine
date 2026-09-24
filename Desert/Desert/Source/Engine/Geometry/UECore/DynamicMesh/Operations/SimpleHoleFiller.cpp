// Ported from UE 5.8 .../DynamicMesh/Private/Operations/SimpleHoleFiller.cpp (see the header for the line ranges
// and what was left out).
#include "Engine/Geometry/UECore/DynamicMesh/Operations/SimpleHoleFiller.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"

#include <spdlog/fmt/fmt.h>

using namespace Desert::Geometry;

bool FSimpleHoleFiller::Fill( int GroupID )
{
    if ( Mesh->HasAttributes() && Mesh->Attributes()->HasPrimaryColors() )
    {
        FailureReason = "SimpleHoleFiller: the mesh has a primary colour layer and the colour fill "
                        "(HoleFillUtil::FillColorOverlay) is not ported";
        return false;
    }
    if ( GroupID < 0 && Mesh->HasTriangleGroups() )
        GroupID = Mesh->AllocateTriangleGroup();

    if ( Loop.GetVertexCount() < 3 )
    {
        FailureReason =
             fmt::format( "SimpleHoleFiller: the loop has {} vertices, 3 needed", Loop.GetVertexCount() );
        return false;
    }

    // a three-vertex hole needs one triangle
    if ( Loop.GetVertexCount() == 3 )
    {
        const FIndex3i Tri( Loop.Vertices[0], Loop.Vertices[2], Loop.Vertices[1] );
        const int      NewTID = Mesh->AppendTriangle( Tri, GroupID );
        if ( NewTID < 0 )
        {
            FailureReason = fmt::format( "SimpleHoleFiller: triangle ({}, {}, {}) could not be appended", Tri.A,
                                         Tri.B, Tri.C );
            return false;
        }
        NewTriangles = { NewTID };
        NewVertex    = IndexConstants::InvalidID;
        return true;
    }
    return Fill_Fan( GroupID );
}

bool FSimpleHoleFiller::Fill_Fan( int GroupID )
{
    FVector3d C = FVector3d::Zero();
    for ( int i = 0; i < Loop.GetVertexCount(); ++i )
        C += Mesh->GetVertex( Loop.Vertices[i] );
    C *= 1.0 / Loop.GetVertexCount();

    NewVertex = Mesh->AppendVertex( C );

    FDynamicMeshEditor     Editor( Mesh );
    FDynamicMeshEditResult AddFanResult;
    if ( !Editor.AddTriangleFan_OrderedVertexLoop( NewVertex, Loop.Vertices, GroupID, AddFanResult ) )
    {
        Mesh->RemoveVertex( NewVertex, false );
        FailureReason =
             fmt::format( "SimpleHoleFiller: the fan around vertex {} over a {}-vertex loop could not be "
                          "appended (a loop edge already has two triangles)",
                          NewVertex, Loop.GetVertexCount() );
        NewVertex = IndexConstants::InvalidID;
        return false;
    }
    NewTriangles = AddFanResult.NewTriangles;
    return true;
}

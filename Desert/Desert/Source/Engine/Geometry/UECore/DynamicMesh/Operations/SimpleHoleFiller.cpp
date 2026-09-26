// Ported from UE 5.8 .../DynamicMesh/Private/Operations/SimpleHoleFiller.cpp (see the header for the line ranges
// and what was left out).
#include "Engine/Geometry/UECore/DynamicMesh/Operations/SimpleHoleFiller.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"

#include <spdlog/fmt/fmt.h>

using namespace Desert::Geometry;

bool SimpleHoleFiller::Fill( int GroupID )
{
    if ( m_Mesh->HasAttributes() && m_Mesh->Attributes()->HasPrimaryColors() )
    {
        m_FailureReason = "SimpleHoleFiller: the mesh has a primary colour layer and the colour fill "
                          "(HoleFillUtil::FillColorOverlay) is not ported";
        return false;
    }
    if ( GroupID < 0 && m_Mesh->HasTriangleGroups() )
        GroupID = m_Mesh->AllocateTriangleGroup();

    if ( m_Loop.GetVertexCount() < 3 )
    {
        m_FailureReason =
             fmt::format( "SimpleHoleFiller: the loop has {} vertices, 3 needed", m_Loop.GetVertexCount() );
        return false;
    }

    // a three-vertex hole needs one triangle
    if ( m_Loop.GetVertexCount() == 3 )
    {
        const Index3i Tri( m_Loop.Vertices[0], m_Loop.Vertices[2], m_Loop.Vertices[1] );
        const int     NewTID = m_Mesh->AppendTriangle( Tri, GroupID );
        if ( NewTID < 0 )
        {
            m_FailureReason = fmt::format( "SimpleHoleFiller: triangle ({}, {}, {}) could not be appended", Tri.A,
                                           Tri.B, Tri.C );
            return false;
        }
        m_NewTriangles = { NewTID };
        m_NewVertex    = IndexConstants::InvalidID;
        return true;
    }
    return Fill_Fan( GroupID );
}

bool SimpleHoleFiller::Fill_Fan( int GroupID )
{
    auto C = glm::dvec3( 0 );
    for ( int i = 0; i < m_Loop.GetVertexCount(); ++i )
        C += m_Mesh->GetVertex( m_Loop.Vertices[i] );
    C *= 1.0 / m_Loop.GetVertexCount();

    m_NewVertex = m_Mesh->AppendVertex( C );

    DynamicMeshEditor     Editor( m_Mesh );
    DynamicMeshEditResult AddFanResult;
    if ( !Editor.AddTriangleFan_OrderedVertexLoop( m_NewVertex, m_Loop.Vertices, GroupID, AddFanResult ) )
    {
        m_Mesh->RemoveVertex( m_NewVertex, false );
        m_FailureReason =
             fmt::format( "SimpleHoleFiller: the fan around vertex {} over a {}-vertex loop could not be "
                          "appended (a loop edge already has two triangles)",
                          m_NewVertex, m_Loop.GetVertexCount() );
        m_NewVertex = IndexConstants::InvalidID;
        return false;
    }
    m_NewTriangles = AddFanResult.NewTriangles;
    return true;
}

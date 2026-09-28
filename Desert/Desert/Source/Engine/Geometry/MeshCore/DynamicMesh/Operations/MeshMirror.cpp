// Ported from UE 5.8 .../DynamicMesh/Private/Operations/MeshMirror.cpp (see the header for the line ranges and
// what was adapted).
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MeshMirror.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Desert::Geometry;

namespace
{
    glm::vec3 MirrorNormal( const glm::vec3& OldNormal, const glm::dvec3& PlaneNormal )
    {
        return OldNormal - glm::vec3( 2.0 * glm::dot( PlaneNormal, glm::dvec3( OldNormal ) ) * PlaneNormal );
    }

    VertexInfo CreateMirrorVertex( const VertexInfo& OriginalVertexInfo, const glm::dvec3& PlaneNormal,
                                   double SignedDistance )
    {
        VertexInfo MirrorVertexInfo( OriginalVertexInfo );
        MirrorVertexInfo.Position = OriginalVertexInfo.Position - 2.0 * SignedDistance * PlaneNormal;
        if ( MirrorVertexInfo.bHaveN )
            MirrorVertexInfo.Normal = MirrorNormal( MirrorVertexInfo.Normal, PlaneNormal );
        // Color and UV don't need adjustment.
        return MirrorVertexInfo;
    }

    VertexInfo CreateMirrorVertex( const VertexInfo& OriginalVertexInfo, const glm::dvec3& PlaneNormal,
                                   const glm::dvec3& PlaneOrigin )
    {
        return CreateMirrorVertex( OriginalVertexInfo, PlaneNormal,
                                   glm::dot( OriginalVertexInfo.Position - PlaneOrigin, PlaneNormal ) );
    }

    std::vector<int> OriginalVertexIds( const DynamicMesh3& Mesh )
    {
        std::vector<int> Ids;
        Ids.reserve( static_cast<size_t>( Mesh.VertexCount() ) );
        for ( const int Vid : Mesh.VertexIndicesItr() )
            Ids.push_back( Vid );
        return Ids;
    }

    void MirrorAndAppendVertices( DynamicMesh3& Mesh, const glm::dvec3& PlaneNormal, const glm::dvec3& PlaneOrigin,
                                  std::vector<int>& OutVidToMirrorMap )
    {
        OutVidToMirrorMap.assign( static_cast<size_t>( Mesh.MaxVertexID() ), IndexConstants::InvalidID );
        for ( const int Vid : OriginalVertexIds( Mesh ) )
            OutVidToMirrorMap[Vid] =
                 Mesh.AppendVertex( CreateMirrorVertex( Mesh.GetVertexInfo( Vid ), PlaneNormal, PlaneOrigin ) );
    }

    // Mirrors every vertex off the plane; a vertex on it maps to itself (welded) unless welding would make an edge
    // with more than two faces or it is part of a face in the plane - then it is duplicated.
    void MirrorAndAppendVerticesWithWelding( DynamicMesh3& Mesh, const glm::dvec3& PlaneNormal,
                                             const glm::dvec3& PlaneOrigin, std::vector<int>& OutVidToMirrorMap,
                                             bool bAllowBowtieVertexCreation, double PlaneTolerance,
                                             MeshMirrorNormalMode WeldNormalMode )
    {
        OutVidToMirrorMap.assign( static_cast<size_t>( Mesh.MaxVertexID() ), IndexConstants::InvalidID );
        std::unordered_set<int> VerticesOnPlane;
        for ( const int Vid : OriginalVertexIds( Mesh ) )
        {
            const double SignedDistanceFromPlane = glm::dot( Mesh.GetVertex( Vid ) - PlaneOrigin, PlaneNormal );
            if ( std::abs( SignedDistanceFromPlane ) < PlaneTolerance )
            {
                OutVidToMirrorMap[Vid] = Vid;
                VerticesOnPlane.insert( Vid );
            }
            else
                OutVidToMirrorMap[Vid] = Mesh.AppendVertex(
                     CreateMirrorVertex( Mesh.GetVertexInfo( Vid ), PlaneNormal, SignedDistanceFromPlane ) );
        }

        const bool bHasVertexNormals    = Mesh.HasVertexNormals();
        const bool bHasAttributeNormals = Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals() != nullptr;
        // Iterated in ID order (UE iterates a TSet): the outcome per vertex does not depend on the order.
        std::vector<int> OnPlane( VerticesOnPlane.begin(), VerticesOnPlane.end() );
        std::sort( OnPlane.begin(), OnPlane.end() );
        for ( const int Vid : OnPlane )
        {
            // 1. A vertex on no edge in the plane would become a bowtie: duplicated unless bowties are allowed.
            // 2. A vertex on an interior edge in the plane must be duplicated: no more triangles can attach there.
            // 3. A vertex on boundary edges in the plane is still duplicated if one of those edges' triangles lies
            //    in the plane.
            // 4. Otherwise it is welded.
            bool bForcedToDuplicate     = false;
            bool bAtLeastOneEdgeInPlane = false;
            for ( const int EdgeId : Mesh.VtxEdgesItr( Vid ) )
            {
                const DynamicMesh3::Edge Edge       = Mesh.GetEdge( EdgeId );
                const int                NeighborId = ( Edge.Vert.A == Vid ) ? Edge.Vert.B : Edge.Vert.A;
                if ( !VerticesOnPlane.contains( NeighborId ) )
                    continue;
                bAtLeastOneEdgeInPlane = true;
                if ( Edge.Tri.B != DynamicMesh3::InvalidID )
                {
                    bForcedToDuplicate = true;
                    break;
                }
                const Index3i VertIndices      = Mesh.GetTriangle( Edge.Tri.A );
                int           OppositeVertexId = VertIndices.A;
                if ( OppositeVertexId == Vid )
                    OppositeVertexId = ( VertIndices.B == NeighborId ) ? VertIndices.C : VertIndices.B;
                else if ( OppositeVertexId == NeighborId )
                    OppositeVertexId = ( VertIndices.B == Vid ) ? VertIndices.C : VertIndices.B;
                if ( VerticesOnPlane.contains( OppositeVertexId ) )
                {
                    bForcedToDuplicate = true;
                    break;
                }
            }

            if ( bForcedToDuplicate || ( !bAtLeastOneEdgeInPlane && !bAllowBowtieVertexCreation ) )
            {
                OutVidToMirrorMap[Vid] = Mesh.AppendVertex(
                     CreateMirrorVertex( Mesh.GetVertexInfo( Vid ), PlaneNormal, PlaneOrigin ) );
                continue;
            }
            // Welded: moved exactly onto the plane, its normal (optionally) projected onto the plane.
            const glm::dvec3 OldPosition = Mesh.GetVertex( Vid );
            Mesh.SetVertex( Vid, OldPosition - glm::dot( PlaneNormal, OldPosition - PlaneOrigin ) * PlaneNormal );
            if ( WeldNormalMode != MeshMirrorNormalMode::AverageMirrorNormals )
                continue;
            const glm::vec3 PlaneNormalF( PlaneNormal );
            // A bowtie's normal may project to zero; it is meaningless there anyway, so it is left as it is.
            auto ComputeVertexNormal = [&PlaneNormalF]( const glm::vec3& OldNormal, glm::vec3& NewNormal )
            {
                NewNormal = OldNormal - glm::dot( OldNormal, PlaneNormalF ) * PlaneNormalF;
                return Normalize( NewNormal ) != 0.0f;
            };
            if ( bHasVertexNormals )
            {
                glm::vec3 NewNormal;
                if ( ComputeVertexNormal( Mesh.GetVertexNormal( Vid ), NewNormal ) )
                    Mesh.SetVertexNormal( Vid, NewNormal );
            }
            if ( bHasAttributeNormals )
            {
                DynamicMeshNormalOverlay*              NormalOverlay = Mesh.Attributes()->PrimaryNormals();
                std::vector<std::pair<int, glm::vec3>> Updates;
                NormalOverlay->EnumerateVertexElements( Vid,
                                                        [&]( int, int ElementId, const glm::vec3& OldNormal )
                                                        {
                                                            glm::vec3 NewNormal;
                                                            if ( ComputeVertexNormal( OldNormal, NewNormal ) )
                                                                Updates.emplace_back( ElementId, NewNormal );
                                                            return true;
                                                        } );
                for ( const auto& [ElementId, NewNormal] : Updates )
                    NormalOverlay->SetElement( ElementId, NewNormal );
            }
        }
    }

    // FDynamicMeshEditor::CopyAttributes with FMeshIndexMappings: each source overlay element is duplicated once;
    // the new normal-layer elements are recorded so they can be mirrored.
    struct AttributeCopier
    {
        std::vector<std::unordered_map<int, int>> UVMaps;
        std::vector<std::unordered_map<int, int>> NormalMaps;
        std::unordered_map<int, int>              ColorMap;
        std::vector<std::vector<int>>             NewNormalOverlayElements;

        explicit AttributeCopier( const DynamicMeshAttributeSet& Attributes )
             : UVMaps( static_cast<size_t>( Attributes.NumUVLayers() ) ),
               NormalMaps( static_cast<size_t>( Attributes.NumNormalLayers() ) ),
               NewNormalOverlayElements( static_cast<size_t>( Attributes.NumNormalLayers() ) )
        {
        }

        template <typename OverlayType>
        static void CopyOverlay( OverlayType* Overlay, int FromTid, int ToTid, std::unordered_map<int, int>& Map,
                                 std::vector<int>* NewElements )
        {
            if ( Overlay == nullptr || !Overlay->IsSetTriangle( FromTid ) )
                return;
            const Index3i Elems = Overlay->GetTriangle( FromTid );
            Index3i       NewElems;
            for ( int j = 0; j < 3; ++j )
            {
                const auto Found = Map.find( Elems[j] );
                if ( Found != Map.end() )
                {
                    NewElems[j] = Found->second;
                    continue;
                }
                NewElems[j] = Overlay->AppendElement( Overlay->GetElement( Elems[j] ) );
                Map.emplace( Elems[j], NewElems[j] );
                if ( NewElements )
                    NewElements->push_back( NewElems[j] );
            }
            Overlay->SetTriangle( ToTid, NewElems );
        }

        void Copy( DynamicMeshAttributeSet& Attributes, int FromTid, int ToTid )
        {
            for ( int k = 0; k < Attributes.NumUVLayers(); ++k )
                CopyOverlay( Attributes.GetUVLayer( k ), FromTid, ToTid, UVMaps[k], nullptr );
            for ( int k = 0; k < Attributes.NumNormalLayers(); ++k )
                CopyOverlay( Attributes.GetNormalLayer( k ), FromTid, ToTid, NormalMaps[k],
                             &NewNormalOverlayElements[k] );
            if ( Attributes.HasPrimaryColors() )
                CopyOverlay( Attributes.PrimaryColors(), FromTid, ToTid, ColorMap, nullptr );
            if ( Attributes.HasMaterialID() )
                Attributes.GetMaterialID()->SetValue( ToTid, Attributes.GetMaterialID()->GetValue( FromTid ) );
        }
    };
} // namespace

void MeshMirror::Mirror()
{
    Normalize( m_PlaneNormal );
    for ( const int Vid : m_Mesh->VertexIndicesItr() )
    {
        const glm::dvec3 OldPosition = m_Mesh->GetVertex( Vid );
        m_Mesh->SetVertex( Vid, OldPosition - 2.0 * glm::dot( m_PlaneNormal, OldPosition - m_PlaneOrigin ) *
                                                   m_PlaneNormal );
        if ( m_Mesh->HasVertexNormals() )
            m_Mesh->SetVertexNormal( Vid, MirrorNormal( m_Mesh->GetVertexNormal( Vid ), m_PlaneNormal ) );
    }
    for ( const int Tid : m_Mesh->TriangleIndicesItr() )
        static_cast<void>( m_Mesh->ReverseTriOrientation( Tid ) );
    if ( !m_Mesh->HasAttributes() )
        return;
    // Normal layers (tangents included) are the only attributes that need mirroring.
    DynamicMeshAttributeSet* Attributes = m_Mesh->Attributes();
    for ( int LayerIndex = 0; LayerIndex < Attributes->NumNormalLayers(); ++LayerIndex )
    {
        DynamicMeshNormalOverlay* NormalOverlay = Attributes->GetNormalLayer( LayerIndex );
        for ( const int i : NormalOverlay->ElementIndicesItr() )
            NormalOverlay->SetElement( i, MirrorNormal( NormalOverlay->GetElement( i ), m_PlaneNormal ) );
    }
}

bool MeshMirror::MirrorAndAppend()
{
    m_FailedTriangle = DynamicMesh3::InvalidID;
    Normalize( m_PlaneNormal );
    // Each original vertex -> its mirror (itself when welded).
    std::vector<int> VidToMirrorMap;
    if ( m_bWeldAlongPlane )
        MirrorAndAppendVerticesWithWelding( *m_Mesh, m_PlaneNormal, m_PlaneOrigin, VidToMirrorMap,
                                            m_bAllowBowtieVertexCreation, m_PlaneTolerance, m_WeldNormalMode );
    else
        MirrorAndAppendVertices( *m_Mesh, m_PlaneNormal, m_PlaneOrigin, VidToMirrorMap );

    std::vector<int> OriginalTriangleIndices;
    OriginalTriangleIndices.reserve( static_cast<size_t>( m_Mesh->TriangleCount() ) );
    for ( const int Tid : m_Mesh->TriangleIndicesItr() )
        OriginalTriangleIndices.push_back( Tid );

    std::unordered_map<int, int>   GroupMapping;
    DynamicMeshAttributeSet*       Attributes = m_Mesh->HasAttributes() ? m_Mesh->Attributes() : nullptr;
    std::optional<AttributeCopier> Copier;
    if ( Attributes )
        Copier.emplace( *Attributes );
    for ( const int Tid : OriginalTriangleIndices )
    {
        const Index3i OriginalTriangle = m_Mesh->GetTriangle( Tid );
        const Index3i Mirrored( VidToMirrorMap[OriginalTriangle.A], VidToMirrorMap[OriginalTriangle.B],
                                VidToMirrorMap[OriginalTriangle.C] );
        int           NewTid = DynamicMesh3::InvalidID;
        if ( !m_Mesh->HasTriangleGroups() )
            NewTid = m_Mesh->AppendTriangle( Mirrored );
        else
        {
            const int Gid   = m_Mesh->GetTriangleGroup( Tid );
            auto      Found = GroupMapping.find( Gid );
            if ( Found == GroupMapping.end() )
                Found = GroupMapping.emplace( Gid, m_Mesh->AllocateTriangleGroup() ).first;
            NewTid = m_Mesh->AppendTriangle( Mirrored, Found->second );
        }
        // UE check()s this: the welding rules leave no edge that could take a third triangle - unless the input
        // already crosses the plane (Append without cropping first). Reported, not asserted.
        if ( NewTid < 0 )
        {
            m_FailedTriangle = Tid;
            m_FailureCode    = NewTid;
            return false;
        }
        // Attributes are copied before the reversal so that they get reversed with the triangle.
        if ( Copier )
            Copier->Copy( *Attributes, Tid, NewTid );
        static_cast<void>( m_Mesh->ReverseTriOrientation( NewTid ) );
    }

    if ( !Copier )
        return true;
    // The copied normals (tangents included) are mirrored.
    for ( int LayerIndex = 0; LayerIndex < static_cast<int>( Copier->NewNormalOverlayElements.size() );
          ++LayerIndex )
    {
        DynamicMeshNormalOverlay* NormalOverlay = Attributes->GetNormalLayer( LayerIndex );
        for ( const int ElementIndex : Copier->NewNormalOverlayElements[LayerIndex] )
            NormalOverlay->SetElement( ElementIndex,
                                       MirrorNormal( NormalOverlay->GetElement( ElementIndex ), m_PlaneNormal ) );
    }
    return true;
}

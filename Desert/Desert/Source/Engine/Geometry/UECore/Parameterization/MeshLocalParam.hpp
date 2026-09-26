// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Parameterization/MeshLocalParam.h
// (LocalParamTypes, ComputeToMaxDistance from a centre vertex, HasUV, GetUV, ProcessQueueUntilTermination,
// ComputeLocalUV, PropagateUV, UpdateUVExpmap/Upwind/Planar, UpdateNeighboursSparse), adapted: nodes addressed by
// index in a TArray (see MeshDijkstra.hpp), the 2x2 rotation written out (no FMatrix2d), external normals and the
// three-seed / TransformUV / GetAllComputedUVs entry points are not ported. Normals: the mesh's vertex normals
// when it has them, else MeshNormals::ComputeVertexNormal, as UE's GetNormal for DynamicMesh3.
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"
#include "Engine/Geometry/UECore/FrameTypes.hpp"
#include "Engine/Geometry/UECore/IndexPriorityQueue.hpp"

namespace Desert::Geometry
{
    enum class LocalParamTypes : uint8_t
    {
        PlanarProjection        = 1,
        ExponentialMap          = 2,
        ExponentialMapUpwindAvg = 3
    };

    template <class PointSetType>
    class MeshLocalParam
    {
    public:
        LocalParamTypes m_ParamMode = LocalParamTypes::ExponentialMapUpwindAvg;

        explicit MeshLocalParam( const PointSetType* PointSetIn ) : m_PointSet( PointSetIn )
        {
            m_Queue.Initialize( m_PointSet->MaxVertexID() );
        }

        void ComputeToMaxDistance( int32_t CenterPointVtxID, const Frame3d& CenterPointFrame,
                                   double ComputeToMaxDistanceIn )
        {
            m_SeedFrame          = CenterPointFrame;
            m_MaxGraphDistance   = 0.0;
            GraphNode& Center    = m_AllocatedNodes[GetNodeIndex( CenterPointVtxID, true )];
            Center.UV            = glm::dvec2( 0 );
            Center.GraphDistance = 0;
            Center.bFrozen       = true;
            m_Queue.Insert( CenterPointVtxID, 0 );
            ProcessQueueUntilTermination( ComputeToMaxDistanceIn );
        }
        [[nodiscard]] bool HasUV( int32_t PointID ) const
        {
            const int32_t* Found = FindValue( m_IDToNodeIndexMap, PointID );
            return Found != nullptr && m_AllocatedNodes[*Found].bFrozen;
        }
        [[nodiscard]] glm::dvec2 GetUV( int32_t PointID ) const
        {
            const int32_t* Found = FindValue( m_IDToNodeIndexMap, PointID );
            return ( Found != nullptr && m_AllocatedNodes[*Found].bFrozen )
                        ? m_AllocatedNodes[*Found].UV
                        : glm::dvec2( std::numeric_limits<double>::max(), std::numeric_limits<double>::max() );
        }

    private:
        struct GraphNode
        {
            int32_t   PointID       = 0;
            int32_t   ParentPointID = 0;
            double    GraphDistance = 0.0;
            glm::dvec2 UV{};
            bool      bFrozen = false;
            glm::dvec3 CachedNormal{};
        };
        const PointSetType*                  m_PointSet;
        std::unordered_map<int32_t, int32_t> m_IDToNodeIndexMap;
        std::vector<GraphNode>               m_AllocatedNodes;
        IndexPriorityQueue                   m_Queue;
        Frame3d                              m_SeedFrame;
        double                               m_MaxGraphDistance = 0.0;

        [[nodiscard]] glm::dvec3 GetPosition( int32_t PointID ) const
        {
            return m_PointSet->GetVertex( PointID );
        }
        [[nodiscard]] glm::dvec3 GetNormal( int32_t PointID ) const
        {
            if ( m_PointSet->HasVertexNormals() )
            {
                const glm::vec3 N = m_PointSet->GetVertexNormal( PointID );
                return { N.x, N.y, N.z };
            }
            return MeshNormals::ComputeVertexNormal( *m_PointSet, PointID );
        }
        Frame3d GetFrame( const GraphNode& Node ) const
        {
            return Frame3d( GetPosition( Node.PointID ), Node.CachedNormal );
        }
        void ProcessQueueUntilTermination( double MaxDistance )
        {
            while ( m_Queue.GetCount() > 0 )
            {
                const int32_t NodeIndex = GetNodeIndex( m_Queue.Dequeue(), false );
                m_MaxGraphDistance =
                     std::max<double>( m_AllocatedNodes[NodeIndex].GraphDistance, m_MaxGraphDistance );
                if ( m_MaxGraphDistance > MaxDistance )
                    return;
                if ( m_AllocatedNodes[NodeIndex].ParentPointID >= 0 )
                {
                    switch ( m_ParamMode )
                    {
                        case LocalParamTypes::ExponentialMap:
                            UpdateUVExpmap( m_AllocatedNodes[NodeIndex] );
                            break;
                        case LocalParamTypes::ExponentialMapUpwindAvg:
                            UpdateUVExpmapUpwind( m_AllocatedNodes[NodeIndex] );
                            break;
                        case LocalParamTypes::PlanarProjection:
                            m_AllocatedNodes[NodeIndex].UV =
                                 ComputeLocalUV( m_SeedFrame, GetPosition( m_AllocatedNodes[NodeIndex].PointID ) );
                            break;
                    }
                }
                m_AllocatedNodes[NodeIndex].bFrozen = true;
                UpdateNeighboursSparse( NodeIndex );
            }
        }
        static glm::dvec2 ComputeLocalUV( const Frame3d& Frame, glm::dvec3 Position )
        {
            Position -= Frame.Origin;
            return { glm::dot( Position, Frame.X() ), glm::dot( Position, Frame.Y() ) };
        }
        // the UV of Position from the neighbour's UV, in the neighbour's tangent frame rotated into the seed's
        static glm::dvec2 PropagateUV( const glm::dvec3& Position, const glm::dvec2& NbrUV,
                                       const Frame3d& NbrFrame, const Frame3d& SeedFrameIn )
        {
            const glm::dvec2 LocalUV = ComputeLocalUV( NbrFrame, Position );
            Frame3d          SeedToLocal( SeedFrameIn );
            SeedToLocal.AlignAxis( 2, NbrFrame.Z() );
            const glm::dvec3 vAlignedSeedX = SeedToLocal.X();
            const glm::dvec3 vLocalX       = NbrFrame.X();
            const double     CosTheta      = glm::dot( vLocalX, vAlignedSeedX );
            double           SinTheta      = std::sqrt( std::max<double>( 1.0 - CosTheta * CosTheta, 0.0 ) );
            if ( glm::dot( glm::cross( vLocalX, vAlignedSeedX ), NbrFrame.Z() ) < 0 )
                SinTheta = -SinTheta;
            // UE FMatrix2d(Cos, Sin, -Sin, Cos) * LocalUV
            return NbrUV + glm::dvec2( CosTheta * LocalUV.x + SinTheta * LocalUV.y,
                                       -SinTheta * LocalUV.x + CosTheta * LocalUV.y );
        }
        void UpdateUVExpmap( GraphNode& Node )
        {
            const GraphNode& Parent = m_AllocatedNodes[m_IDToNodeIndexMap.at( Node.ParentPointID )];
            Node.UV = PropagateUV( GetPosition( Node.PointID ), Parent.UV, GetFrame( Parent ), m_SeedFrame );
        }
        void UpdateUVExpmapUpwind( GraphNode& Node )
        {
            const glm::dvec3 NodePos   = GetPosition( Node.PointID );
            glm::dvec2       AverageUV = glm::dvec2( 0 );
            double          WeightSum = 0;
            for ( const int32_t NbrPointID : m_PointSet->VtxVerticesItr( Node.PointID ) )
            {
                const int32_t* Found = FindValue( m_IDToNodeIndexMap, NbrPointID );
                if ( Found == nullptr || !m_AllocatedNodes[*Found].bFrozen )
                    continue;
                const Frame3d    NbrFrame = GetFrame( m_AllocatedNodes[*Found] );
                const glm::dvec2 NbrUV =
                     PropagateUV( NodePos, m_AllocatedNodes[*Found].UV, NbrFrame, m_SeedFrame );
                const double Weight =
                     1.0 / ( DistanceSquared( NodePos, NbrFrame.Origin ) + ZeroTolerance<double> );
                AverageUV = AverageUV + NbrUV * Weight;
                WeightSum += Weight;
            }
            // the parent is always a frozen neighbour, so WeightSum > 0 (UE check()s NbrCount > 0)
            Node.UV = AverageUV * ( 1.0 / WeightSum );
        }
        int32_t GetNodeIndex( int32_t PointSetID, bool bCreateIfMissing )
        {
            if ( const int32_t* Found = FindValue( m_IDToNodeIndexMap, PointSetID ) )
                return *Found;
            if ( !bCreateIfMissing )
                return -1;
            m_AllocatedNodes.push_back(
                 GraphNode{ PointSetID, -1, 0.0, glm::dvec2( 0 ), false, GetNormal( PointSetID ) } );
            const int32_t NewIndex = static_cast<int32_t>( m_AllocatedNodes.size() ) - 1;
            m_IDToNodeIndexMap.insert_or_assign( PointSetID, NewIndex );
            return NewIndex;
        }
        void UpdateNeighboursSparse( int32_t ParentIndex )
        {
            const int32_t    ParentID   = m_AllocatedNodes[ParentIndex].PointID;
            const double     ParentDist = m_AllocatedNodes[ParentIndex].GraphDistance;
            const glm::dvec3 ParentPos  = GetPosition( ParentID );
            for ( const int32_t NbrPointID : m_PointSet->VtxVerticesItr( ParentID ) )
            {
                GraphNode& Nbr = m_AllocatedNodes[GetNodeIndex( NbrPointID, true )];
                if ( Nbr.bFrozen )
                    continue;
                const double NbrDist = ParentDist + Distance( ParentPos, GetPosition( NbrPointID ) );
                if ( m_Queue.Contains( NbrPointID ) )
                {
                    if ( NbrDist < Nbr.GraphDistance )
                    {
                        Nbr.ParentPointID = ParentID;
                        Nbr.GraphDistance = NbrDist;
                        m_Queue.Update( NbrPointID, static_cast<float>( NbrDist ) );
                    }
                }
                else
                {
                    Nbr.ParentPointID = ParentID;
                    Nbr.GraphDistance = NbrDist;
                    m_Queue.Insert( NbrPointID, static_cast<float>( NbrDist ) );
                }
            }
        }
    };
} // namespace Desert::Geometry

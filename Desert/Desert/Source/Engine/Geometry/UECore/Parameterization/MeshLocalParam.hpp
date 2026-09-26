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
        LocalParamTypes ParamMode = LocalParamTypes::ExponentialMapUpwindAvg;

        explicit MeshLocalParam( const PointSetType* PointSetIn ) : PointSet( PointSetIn )
        {
            Queue.Initialize( PointSet->MaxVertexID() );
        }

        void ComputeToMaxDistance( int32_t CenterPointVtxID, const Frame3d& CenterPointFrame,
                                   double ComputeToMaxDistanceIn )
        {
            SeedFrame            = CenterPointFrame;
            MaxGraphDistance     = 0.0;
            GraphNode& Center    = AllocatedNodes[GetNodeIndex( CenterPointVtxID, true )];
            Center.UV            = glm::dvec2( 0 );
            Center.GraphDistance = 0;
            Center.bFrozen       = true;
            Queue.Insert( CenterPointVtxID, 0 );
            ProcessQueueUntilTermination( ComputeToMaxDistanceIn );
        }
        [[nodiscard]] bool HasUV( int32_t PointID ) const
        {
            const int32_t* Found = FindValue( IDToNodeIndexMap, PointID );
            return Found != nullptr && AllocatedNodes[*Found].bFrozen;
        }
        [[nodiscard]] glm::dvec2 GetUV( int32_t PointID ) const
        {
            const int32_t* Found = FindValue( IDToNodeIndexMap, PointID );
            return ( Found != nullptr && AllocatedNodes[*Found].bFrozen )
                        ? AllocatedNodes[*Found].UV
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
        const PointSetType* PointSet;
        std::unordered_map<int32_t, int32_t> IDToNodeIndexMap;
        std::vector<GraphNode>               AllocatedNodes;
        IndexPriorityQueue                   Queue;
        Frame3d                              SeedFrame;
        double              MaxGraphDistance = 0.0;

        [[nodiscard]] glm::dvec3 GetPosition( int32_t PointID ) const
        {
            return PointSet->GetVertex( PointID );
        }
        [[nodiscard]] glm::dvec3 GetNormal( int32_t PointID ) const
        {
            if ( PointSet->HasVertexNormals() )
            {
                const glm::vec3 N = PointSet->GetVertexNormal( PointID );
                return { N.x, N.y, N.z };
            }
            return MeshNormals::ComputeVertexNormal( *PointSet, PointID );
        }
        Frame3d GetFrame( const GraphNode& Node ) const
        {
            return Frame3d( GetPosition( Node.PointID ), Node.CachedNormal );
        }
        void ProcessQueueUntilTermination( double MaxDistance )
        {
            while ( Queue.GetCount() > 0 )
            {
                const int32_t NodeIndex = GetNodeIndex( Queue.Dequeue(), false );
                MaxGraphDistance =
                     TMathUtil<double>::Max( AllocatedNodes[NodeIndex].GraphDistance, MaxGraphDistance );
                if ( MaxGraphDistance > MaxDistance )
                    return;
                if ( AllocatedNodes[NodeIndex].ParentPointID >= 0 )
                {
                    switch ( ParamMode )
                    {
                        case LocalParamTypes::ExponentialMap:
                            UpdateUVExpmap( AllocatedNodes[NodeIndex] );
                            break;
                        case LocalParamTypes::ExponentialMapUpwindAvg:
                            UpdateUVExpmapUpwind( AllocatedNodes[NodeIndex] );
                            break;
                        case LocalParamTypes::PlanarProjection:
                            AllocatedNodes[NodeIndex].UV =
                                 ComputeLocalUV( SeedFrame, GetPosition( AllocatedNodes[NodeIndex].PointID ) );
                            break;
                    }
                }
                AllocatedNodes[NodeIndex].bFrozen = true;
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
            double          SinTheta      = std::sqrt( TMathUtil<double>::Max( 1.0 - CosTheta * CosTheta, 0.0 ) );
            if ( glm::dot( glm::cross( vLocalX, vAlignedSeedX ), NbrFrame.Z() ) < 0 )
                SinTheta = -SinTheta;
            // UE FMatrix2d(Cos, Sin, -Sin, Cos) * LocalUV
            return NbrUV + glm::dvec2( CosTheta * LocalUV.x + SinTheta * LocalUV.y,
                                       -SinTheta * LocalUV.x + CosTheta * LocalUV.y );
        }
        void UpdateUVExpmap( GraphNode& Node )
        {
            const GraphNode& Parent = AllocatedNodes[IDToNodeIndexMap.at( Node.ParentPointID )];
            Node.UV = PropagateUV( GetPosition( Node.PointID ), Parent.UV, GetFrame( Parent ), SeedFrame );
        }
        void UpdateUVExpmapUpwind( GraphNode& Node )
        {
            const glm::dvec3 NodePos   = GetPosition( Node.PointID );
            glm::dvec2       AverageUV = glm::dvec2( 0 );
            double          WeightSum = 0;
            for ( const int32_t NbrPointID : PointSet->VtxVerticesItr( Node.PointID ) )
            {
                const int32_t* Found = FindValue( IDToNodeIndexMap, NbrPointID );
                if ( Found == nullptr || !AllocatedNodes[*Found].bFrozen )
                    continue;
                const Frame3d    NbrFrame = GetFrame( AllocatedNodes[*Found] );
                const glm::dvec2 NbrUV    = PropagateUV( NodePos, AllocatedNodes[*Found].UV, NbrFrame, SeedFrame );
                const double    Weight =
                     1.0 / ( DistanceSquared( NodePos, NbrFrame.Origin ) + TMathUtil<double>::ZeroTolerance );
                AverageUV = AverageUV + NbrUV * Weight;
                WeightSum += Weight;
            }
            // the parent is always a frozen neighbour, so WeightSum > 0 (UE check()s NbrCount > 0)
            Node.UV = AverageUV * ( 1.0 / WeightSum );
        }
        int32_t GetNodeIndex( int32_t PointSetID, bool bCreateIfMissing )
        {
            if ( const int32_t* Found = FindValue( IDToNodeIndexMap, PointSetID ) )
                return *Found;
            if ( !bCreateIfMissing )
                return -1;
            AllocatedNodes.push_back(
                 GraphNode{ PointSetID, -1, 0.0, glm::dvec2( 0 ), false, GetNormal( PointSetID ) } );
            const int32_t NewIndex = static_cast<int32_t>( AllocatedNodes.size() ) - 1;
            IDToNodeIndexMap.insert_or_assign( PointSetID, NewIndex );
            return NewIndex;
        }
        void UpdateNeighboursSparse( int32_t ParentIndex )
        {
            const int32_t   ParentID   = AllocatedNodes[ParentIndex].PointID;
            const double    ParentDist = AllocatedNodes[ParentIndex].GraphDistance;
            const glm::dvec3 ParentPos  = GetPosition( ParentID );
            for ( const int32_t NbrPointID : PointSet->VtxVerticesItr( ParentID ) )
            {
                GraphNode& Nbr = AllocatedNodes[GetNodeIndex( NbrPointID, true )];
                if ( Nbr.bFrozen )
                    continue;
                const double NbrDist = ParentDist + Distance( ParentPos, GetPosition( NbrPointID ) );
                if ( Queue.Contains( NbrPointID ) )
                {
                    if ( NbrDist < Nbr.GraphDistance )
                    {
                        Nbr.ParentPointID = ParentID;
                        Nbr.GraphDistance = NbrDist;
                        Queue.Update( NbrPointID, static_cast<float>( NbrDist ) );
                    }
                }
                else
                {
                    Nbr.ParentPointID = ParentID;
                    Nbr.GraphDistance = NbrDist;
                    Queue.Insert( NbrPointID, static_cast<float>( NbrDist ) );
                }
            }
        }
    };
} // namespace Desert::Geometry

// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Parameterization/MeshLocalParam.h
// (ELocalParamTypes, ComputeToMaxDistance from a centre vertex, HasUV, GetUV, ProcessQueueUntilTermination,
// ComputeLocalUV, PropagateUV, UpdateUVExpmap/Upwind/Planar, UpdateNeighboursSparse), adapted: nodes addressed by
// index in a TArray (see MeshDijkstra.hpp), the 2x2 rotation written out (no FMatrix2d), external normals and the
// three-seed / TransformUV / GetAllComputedUVs entry points are not ported. Normals: the mesh's vertex normals
// when it has them, else FMeshNormals::ComputeVertexNormal, as UE's GetNormal for FDynamicMesh3.
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/FrameTypes.hpp"
#include "Engine/Geometry/UECore/IndexPriorityQueue.hpp"

namespace Desert::Geometry
{
    enum class ELocalParamTypes : uint8_t
    {
        PlanarProjection        = 1,
        ExponentialMap          = 2,
        ExponentialMapUpwindAvg = 3
    };

    template <class PointSetType>
    class TMeshLocalParam
    {
    public:
        ELocalParamTypes ParamMode = ELocalParamTypes::ExponentialMapUpwindAvg;

        explicit TMeshLocalParam( const PointSetType* PointSetIn ) : PointSet( PointSetIn )
        {
            Queue.Initialize( PointSet->MaxVertexID() );
        }

        void ComputeToMaxDistance( int32_t CenterPointVtxID, const FFrame3d& CenterPointFrame,
                                   double ComputeToMaxDistanceIn )
        {
            SeedFrame            = CenterPointFrame;
            MaxGraphDistance     = 0.0;
            FGraphNode& Center   = AllocatedNodes[GetNodeIndex( CenterPointVtxID, true )];
            Center.UV            = FVector2d::Zero();
            Center.GraphDistance = 0;
            Center.bFrozen       = true;
            Queue.Insert( CenterPointVtxID, 0 );
            ProcessQueueUntilTermination( ComputeToMaxDistanceIn );
        }
        [[nodiscard]] bool HasUV( int32_t PointID ) const
        {
            const int32_t* Found = IDToNodeIndexMap.Find( PointID );
            return Found != nullptr && AllocatedNodes[*Found].bFrozen;
        }
        [[nodiscard]] FVector2d GetUV( int32_t PointID ) const
        {
            const int32_t* Found = IDToNodeIndexMap.Find( PointID );
            return ( Found != nullptr && AllocatedNodes[*Found].bFrozen )
                        ? AllocatedNodes[*Found].UV
                        : FVector2d( std::numeric_limits<double>::max(), std::numeric_limits<double>::max() );
        }

    private:
        struct FGraphNode
        {
            int32_t   PointID       = 0;
            int32_t   ParentPointID = 0;
            double    GraphDistance = 0.0;
            FVector2d UV;
            bool      bFrozen = false;
            FVector3d CachedNormal;
        };
        const PointSetType* PointSet;
        TMap<int32_t, int32_t> IDToNodeIndexMap;
        TArray<FGraphNode>  AllocatedNodes;
        FIndexPriorityQueue Queue;
        FFrame3d            SeedFrame;
        double              MaxGraphDistance = 0.0;

        [[nodiscard]] FVector3d GetPosition( int32_t PointID ) const
        {
            return PointSet->GetVertex( PointID );
        }
        [[nodiscard]] FVector3d GetNormal( int32_t PointID ) const
        {
            if ( PointSet->HasVertexNormals() )
            {
                const FVector3f N = PointSet->GetVertexNormal( PointID );
                return { N.X, N.Y, N.Z };
            }
            return FMeshNormals::ComputeVertexNormal( *PointSet, PointID );
        }
        FFrame3d GetFrame( const FGraphNode& Node ) const
        {
            return FFrame3d( GetPosition( Node.PointID ), Node.CachedNormal );
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
                        case ELocalParamTypes::ExponentialMap:
                            UpdateUVExpmap( AllocatedNodes[NodeIndex] );
                            break;
                        case ELocalParamTypes::ExponentialMapUpwindAvg:
                            UpdateUVExpmapUpwind( AllocatedNodes[NodeIndex] );
                            break;
                        case ELocalParamTypes::PlanarProjection:
                            AllocatedNodes[NodeIndex].UV =
                                 ComputeLocalUV( SeedFrame, GetPosition( AllocatedNodes[NodeIndex].PointID ) );
                            break;
                    }
                }
                AllocatedNodes[NodeIndex].bFrozen = true;
                UpdateNeighboursSparse( NodeIndex );
            }
        }
        static FVector2d ComputeLocalUV( const FFrame3d& Frame, FVector3d Position )
        {
            Position -= Frame.Origin;
            return { Position.Dot( Frame.X() ), Position.Dot( Frame.Y() ) };
        }
        // the UV of Position from the neighbour's UV, in the neighbour's tangent frame rotated into the seed's
        static FVector2d PropagateUV( const FVector3d& Position, const FVector2d& NbrUV, const FFrame3d& NbrFrame,
                                      const FFrame3d& SeedFrameIn )
        {
            const FVector2d LocalUV = ComputeLocalUV( NbrFrame, Position );
            FFrame3d        SeedToLocal( SeedFrameIn );
            SeedToLocal.AlignAxis( 2, NbrFrame.Z() );
            const FVector3d vAlignedSeedX = SeedToLocal.X();
            const FVector3d vLocalX       = NbrFrame.X();
            const double    CosTheta      = vLocalX.Dot( vAlignedSeedX );
            double          SinTheta      = std::sqrt( TMathUtil<double>::Max( 1.0 - CosTheta * CosTheta, 0.0 ) );
            if ( vLocalX.Cross( vAlignedSeedX ).Dot( NbrFrame.Z() ) < 0 )
                SinTheta = -SinTheta;
            // UE FMatrix2d(Cos, Sin, -Sin, Cos) * LocalUV
            return NbrUV + FVector2d( CosTheta * LocalUV.X + SinTheta * LocalUV.Y,
                                      -SinTheta * LocalUV.X + CosTheta * LocalUV.Y );
        }
        void UpdateUVExpmap( FGraphNode& Node )
        {
            const FGraphNode& Parent = AllocatedNodes[*IDToNodeIndexMap.Find( Node.ParentPointID )];
            Node.UV = PropagateUV( GetPosition( Node.PointID ), Parent.UV, GetFrame( Parent ), SeedFrame );
        }
        void UpdateUVExpmapUpwind( FGraphNode& Node )
        {
            const FVector3d NodePos   = GetPosition( Node.PointID );
            FVector2d       AverageUV = FVector2d::Zero();
            double          WeightSum = 0;
            for ( const int32_t NbrPointID : PointSet->VtxVerticesItr( Node.PointID ) )
            {
                const int32_t* Found = IDToNodeIndexMap.Find( NbrPointID );
                if ( Found == nullptr || !AllocatedNodes[*Found].bFrozen )
                    continue;
                const FFrame3d  NbrFrame = GetFrame( AllocatedNodes[*Found] );
                const FVector2d NbrUV    = PropagateUV( NodePos, AllocatedNodes[*Found].UV, NbrFrame, SeedFrame );
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
            if ( const int32_t* Found = IDToNodeIndexMap.Find( PointSetID ) )
                return *Found;
            if ( !bCreateIfMissing )
                return -1;
            const int32_t NewIndex = AllocatedNodes.Add(
                 FGraphNode{ PointSetID, -1, 0.0, FVector2d::Zero(), false, GetNormal( PointSetID ) } );
            IDToNodeIndexMap.Add( PointSetID, NewIndex );
            return NewIndex;
        }
        void UpdateNeighboursSparse( int32_t ParentIndex )
        {
            const int32_t   ParentID   = AllocatedNodes[ParentIndex].PointID;
            const double    ParentDist = AllocatedNodes[ParentIndex].GraphDistance;
            const FVector3d ParentPos  = GetPosition( ParentID );
            for ( const int32_t NbrPointID : PointSet->VtxVerticesItr( ParentID ) )
            {
                FGraphNode& Nbr = AllocatedNodes[GetNodeIndex( NbrPointID, true )];
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

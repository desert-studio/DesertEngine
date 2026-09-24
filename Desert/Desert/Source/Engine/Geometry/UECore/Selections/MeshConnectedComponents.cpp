// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Selections/MeshConnectedComponents.cpp:8-22,
// 94-111, 245-347, adapted: UE Core via UECore.hpp, namespace Desert::Geometry, components are built by value and
// appended (UE news an FComponent into a TIndirectArray), no CPU profiler scopes.
#include "Engine/Geometry/UECore/Selections/MeshConnectedComponents.hpp"

using namespace Desert::Geometry;

namespace
{
    // The ActiveSet stays a uint8 array, as in UE; these name its states.
    enum class EProcessingState : uint8
    {
        Unprocessed = 0,
        InQueue     = 1,
        Done        = 2,
        Invalid     = 255
    };
} // namespace

void FMeshConnectedComponents::FindTrianglesConnectedToSeeds(
     const TArray<int>& SeedTriangles, TFunction<bool( int32, int32 )> TrisConnectedPredicate )
{
    // initial active set contains all valid triangles
    TArray<uint8> ActiveSet;
    const int32   NumTriangles = Mesh->MaxTriangleID();
    ActiveSet.Init( static_cast<uint8>( EProcessingState::Invalid ), NumTriangles );
    for ( int32 Tid = 0; Tid < NumTriangles; ++Tid )
    {
        if ( Mesh->IsTriangle( Tid ) )
        {
            ActiveSet[Tid] = static_cast<uint8>( EProcessingState::Unprocessed );
        }
    }

    FindTriComponents( SeedTriangles, ActiveSet, TrisConnectedPredicate );
}

void FMeshConnectedComponents::FindTriComponents( const TArray<int32>& SeedList, TArray<uint8>& ActiveSet,
                                                  const TFunction<bool( int32, int32 )>& TrisConnectedPredicate )
{
    Components.Empty();

    TArray<int32> ComponentQueue;
    ComponentQueue.Reserve( 256 );

    // keep finding valid seed triangles and growing connected components until we are done
    for ( int32 SeedTri : SeedList )
    {
        if ( ActiveSet.IsValidIndex( SeedTri ) &&
             ActiveSet[SeedTri] != static_cast<uint8>( EProcessingState::Invalid ) )
        {
            ComponentQueue.Add( SeedTri );
            ActiveSet[SeedTri] = static_cast<uint8>( EProcessingState::InQueue );

            FComponent Component;
            if ( TrisConnectedPredicate )
            {
                FindTriComponent( Component, ComponentQueue, ActiveSet, TrisConnectedPredicate );
            }
            else
            {
                FindTriComponent( Component, ComponentQueue, ActiveSet );
            }
            RemoveFromActiveSet( Component, ActiveSet );
            Components.Add( std::move( Component ) );

            ComponentQueue.Reset( 0 );
        }
    }
}

void FMeshConnectedComponents::FindTriComponent( FComponent& Component, TArray<int32>& ComponentQueue,
                                                 TArray<uint8>& ActiveSet )
{
    while ( ComponentQueue.Num() > 0 )
    {
        const int32 CurTriangle = ComponentQueue.Pop( EAllowShrinking::No );

        ActiveSet[CurTriangle] = static_cast<uint8>( EProcessingState::Done );
        Component.Indices.Add( CurTriangle );

        const FIndex3i TriNbrTris = Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != FDynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8>( EProcessingState::Unprocessed ) )
            {
                ComponentQueue.Add( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8>( EProcessingState::InQueue );
            }
        }
    }
}

void FMeshConnectedComponents::FindTriComponent( FComponent& Component, TArray<int32>& ComponentQueue,
                                                 TArray<uint8>&                         ActiveSet,
                                                 const TFunction<bool( int32, int32 )>& TriConnectedPredicate )
{
    while ( ComponentQueue.Num() > 0 )
    {
        const int32 CurTriangle = ComponentQueue.Pop( EAllowShrinking::No );

        ActiveSet[CurTriangle] = static_cast<uint8>( EProcessingState::Done );
        Component.Indices.Add( CurTriangle );

        const FIndex3i TriNbrTris = Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != FDynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8>( EProcessingState::Unprocessed ) &&
                 TriConnectedPredicate( CurTriangle, NbrTri ) )
            {
                ComponentQueue.Add( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8>( EProcessingState::InQueue );
            }
        }
    }
}

void FMeshConnectedComponents::RemoveFromActiveSet( const FComponent& Component, TArray<uint8>& ActiveSet )
{
    for ( int32 Tid : Component.Indices )
    {
        ActiveSet[Tid] = static_cast<uint8>( EProcessingState::Invalid );
    }
}

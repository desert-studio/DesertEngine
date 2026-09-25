// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Selections/MeshConnectedComponents.cpp:8-22,
// 94-111, 245-347, adapted: UE Core via UECore.hpp, namespace Desert::Geometry, components are built by value and
// appended (UE news an FComponent into a TIndirectArray), no CPU profiler scopes.
#include "Engine/Geometry/UECore/Selections/MeshConnectedComponents.hpp"

using namespace Desert::Geometry;

namespace
{
    // The ActiveSet stays a uint8_t array, as in UE; these name its states.
    enum class EProcessingState : uint8_t
    {
        Unprocessed = 0,
        InQueue     = 1,
        Done        = 2,
        Invalid     = 255
    };
} // namespace

void FMeshConnectedComponents::FindTrianglesConnectedToSeeds(
     const TArray<int>& SeedTriangles, const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate )
{
    // initial active set contains all valid triangles
    TArray<uint8_t> ActiveSet;
    const int32_t   NumTriangles = Mesh->MaxTriangleID();
    ActiveSet.Init( static_cast<uint8_t>( EProcessingState::Invalid ), NumTriangles );
    for ( int32_t Tid = 0; Tid < NumTriangles; ++Tid )
    {
        if ( Mesh->IsTriangle( Tid ) )
        {
            ActiveSet[Tid] = static_cast<uint8_t>( EProcessingState::Unprocessed );
        }
    }

    FindTriComponents( SeedTriangles, ActiveSet, TrisConnectedPredicate );
}

void FMeshConnectedComponents::FindTriComponents(
     const TArray<int32_t>& SeedList, TArray<uint8_t>& ActiveSet,
     const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate )
{
    Components.Empty();

    TArray<int32_t> ComponentQueue;
    ComponentQueue.Reserve( 256 );

    // keep finding valid seed triangles and growing connected components until we are done
    for ( int32_t const SeedTri : SeedList )
    {
        if ( ActiveSet.IsValidIndex( SeedTri ) &&
             ActiveSet[SeedTri] != static_cast<uint8_t>( EProcessingState::Invalid ) )
        {
            ComponentQueue.Add( SeedTri );
            ActiveSet[SeedTri] = static_cast<uint8_t>( EProcessingState::InQueue );

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

void FMeshConnectedComponents::FindTriComponent( FComponent& Component, TArray<int32_t>& ComponentQueue,
                                                 TArray<uint8_t>& ActiveSet ) const
{
    while ( ComponentQueue.Num() > 0 )
    {
        const int32_t CurTriangle = ComponentQueue.Pop( EAllowShrinking::No );

        ActiveSet[CurTriangle] = static_cast<uint8_t>( EProcessingState::Done );
        Component.Indices.Add( CurTriangle );

        const FIndex3i TriNbrTris = Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != FDynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8_t>( EProcessingState::Unprocessed ) )
            {
                ComponentQueue.Add( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8_t>( EProcessingState::InQueue );
            }
        }
    }
}

void FMeshConnectedComponents::FindTriComponent(
     FComponent& Component, TArray<int32_t>& ComponentQueue, TArray<uint8_t>& ActiveSet,
     const std::function<bool( int32_t, int32_t )>& TriConnectedPredicate ) const
{
    while ( ComponentQueue.Num() > 0 )
    {
        const int32_t CurTriangle = ComponentQueue.Pop( EAllowShrinking::No );

        ActiveSet[CurTriangle] = static_cast<uint8_t>( EProcessingState::Done );
        Component.Indices.Add( CurTriangle );

        const FIndex3i TriNbrTris = Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != FDynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8_t>( EProcessingState::Unprocessed ) &&
                 TriConnectedPredicate( CurTriangle, NbrTri ) )
            {
                ComponentQueue.Add( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8_t>( EProcessingState::InQueue );
            }
        }
    }
}

void FMeshConnectedComponents::RemoveFromActiveSet( const FComponent& Component, TArray<uint8_t>& ActiveSet )
{
    for ( int32_t const Tid : Component.Indices )
    {
        ActiveSet[Tid] = static_cast<uint8_t>( EProcessingState::Invalid );
    }
}

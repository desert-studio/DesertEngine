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
     const std::vector<int>& SeedTriangles, const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate )
{
    // initial active set contains all valid triangles
    std::vector<uint8_t> ActiveSet;
    const int32_t   NumTriangles = Mesh->MaxTriangleID();
    ActiveSet.assign( NumTriangles, static_cast<uint8_t>( EProcessingState::Invalid ) );
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
     const std::vector<int32_t>& SeedList, std::vector<uint8_t>& ActiveSet,
     const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate )
{
    Components.clear();

    std::vector<int32_t> ComponentQueue;
    ComponentQueue.reserve( 256 );

    // keep finding valid seed triangles and growing connected components until we are done
    for ( int32_t const SeedTri : SeedList )
    {
        if ( ( SeedTri >= 0 && SeedTri < static_cast<int32_t>( ActiveSet.size() ) ) &&
             ActiveSet[SeedTri] != static_cast<uint8_t>( EProcessingState::Invalid ) )
        {
            ComponentQueue.push_back( SeedTri );
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
            Components.push_back( std::move( Component ) );

            ComponentQueue.clear();
        }
    }
}

void FMeshConnectedComponents::FindTriComponent( FComponent& Component, std::vector<int32_t>& ComponentQueue,
                                                 std::vector<uint8_t>& ActiveSet ) const
{
    while ( !ComponentQueue.empty() )
    {
        const int32_t CurTriangle = ComponentQueue.back();
        ComponentQueue.pop_back();

        ActiveSet[CurTriangle] = static_cast<uint8_t>( EProcessingState::Done );
        Component.Indices.push_back( CurTriangle );

        const FIndex3i TriNbrTris = Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != FDynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8_t>( EProcessingState::Unprocessed ) )
            {
                ComponentQueue.push_back( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8_t>( EProcessingState::InQueue );
            }
        }
    }
}

void FMeshConnectedComponents::FindTriComponent(
     FComponent& Component, std::vector<int32_t>& ComponentQueue, std::vector<uint8_t>& ActiveSet,
     const std::function<bool( int32_t, int32_t )>& TriConnectedPredicate ) const
{
    while ( !ComponentQueue.empty() )
    {
        const int32_t CurTriangle = ComponentQueue.back();
        ComponentQueue.pop_back();

        ActiveSet[CurTriangle] = static_cast<uint8_t>( EProcessingState::Done );
        Component.Indices.push_back( CurTriangle );

        const FIndex3i TriNbrTris = Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != FDynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8_t>( EProcessingState::Unprocessed ) &&
                 TriConnectedPredicate( CurTriangle, NbrTri ) )
            {
                ComponentQueue.push_back( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8_t>( EProcessingState::InQueue );
            }
        }
    }
}

void FMeshConnectedComponents::RemoveFromActiveSet( const FComponent& Component, std::vector<uint8_t>& ActiveSet )
{
    for ( int32_t const Tid : Component.Indices )
    {
        ActiveSet[Tid] = static_cast<uint8_t>( EProcessingState::Invalid );
    }
}

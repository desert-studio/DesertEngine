// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Selections/MeshConnectedComponents.cpp:8-22,
// 94-111, 245-347, adapted: UE Core as std/glm, namespace Desert::Geometry, components are built by value and
// appended (UE news an Component into a TIndirectArray), no CPU profiler scopes.
#include "Engine/Geometry/MeshCore/Selections/MeshConnectedComponents.hpp"

using namespace Desert::Geometry;

namespace
{
    // The ActiveSet stays a uint8_t array, as in UE; these name its states.
    enum class ProcessingState : uint8_t
    {
        Unprocessed = 0,
        InQueue     = 1,
        Done        = 2,
        Invalid     = 255
    };
} // namespace

void MeshConnectedComponents::FindTrianglesConnectedToSeeds(
     const std::vector<int>& SeedTriangles, const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate )
{
    // initial active set contains all valid triangles
    std::vector<uint8_t> ActiveSet;
    const int32_t        NumTriangles = m_Mesh->MaxTriangleID();
    ActiveSet.assign( NumTriangles, static_cast<uint8_t>( ProcessingState::Invalid ) );
    for ( int32_t Tid = 0; Tid < NumTriangles; ++Tid )
    {
        if ( m_Mesh->IsTriangle( Tid ) )
        {
            ActiveSet[Tid] = static_cast<uint8_t>( ProcessingState::Unprocessed );
        }
    }

    FindTriComponents( SeedTriangles, ActiveSet, TrisConnectedPredicate );
}

void MeshConnectedComponents::FindTriComponents(
     const std::vector<int32_t>& SeedList, std::vector<uint8_t>& ActiveSet,
     const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate )
{
    m_Components.clear();

    std::vector<int32_t> ComponentQueue;
    ComponentQueue.reserve( 256 );

    // keep finding valid seed triangles and growing connected components until we are done
    for ( int32_t const SeedTri : SeedList )
    {
        if ( ( SeedTri >= 0 && SeedTri < static_cast<int32_t>( ActiveSet.size() ) ) &&
             ActiveSet[SeedTri] != static_cast<uint8_t>( ProcessingState::Invalid ) )
        {
            ComponentQueue.push_back( SeedTri );
            ActiveSet[SeedTri] = static_cast<uint8_t>( ProcessingState::InQueue );

            Component Component;
            if ( TrisConnectedPredicate )
            {
                FindTriComponent( Component, ComponentQueue, ActiveSet, TrisConnectedPredicate );
            }
            else
            {
                FindTriComponent( Component, ComponentQueue, ActiveSet );
            }
            RemoveFromActiveSet( Component, ActiveSet );
            m_Components.push_back( std::move( Component ) );

            ComponentQueue.clear();
        }
    }
}

void MeshConnectedComponents::FindTriComponent( Component& Component, std::vector<int32_t>& ComponentQueue,
                                                std::vector<uint8_t>& ActiveSet ) const
{
    while ( !ComponentQueue.empty() )
    {
        const int32_t CurTriangle = ComponentQueue.back();
        ComponentQueue.pop_back();

        ActiveSet[CurTriangle] = static_cast<uint8_t>( ProcessingState::Done );
        Component.Indices.push_back( CurTriangle );

        const Index3i TriNbrTris = m_Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != DynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8_t>( ProcessingState::Unprocessed ) )
            {
                ComponentQueue.push_back( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8_t>( ProcessingState::InQueue );
            }
        }
    }
}

void MeshConnectedComponents::FindTriComponent(
     Component& Component, std::vector<int32_t>& ComponentQueue, std::vector<uint8_t>& ActiveSet,
     const std::function<bool( int32_t, int32_t )>& TriConnectedPredicate ) const
{
    while ( !ComponentQueue.empty() )
    {
        const int32_t CurTriangle = ComponentQueue.back();
        ComponentQueue.pop_back();

        ActiveSet[CurTriangle] = static_cast<uint8_t>( ProcessingState::Done );
        Component.Indices.push_back( CurTriangle );

        const Index3i TriNbrTris = m_Mesh->GetTriNeighbourTris( CurTriangle );
        for ( int j = 0; j < 3; ++j )
        {
            const int NbrTri = TriNbrTris[j];
            if ( NbrTri != DynamicMesh3::InvalidID &&
                 ActiveSet[NbrTri] == static_cast<uint8_t>( ProcessingState::Unprocessed ) &&
                 TriConnectedPredicate( CurTriangle, NbrTri ) )
            {
                ComponentQueue.push_back( NbrTri );
                ActiveSet[NbrTri] = static_cast<uint8_t>( ProcessingState::InQueue );
            }
        }
    }
}

void MeshConnectedComponents::RemoveFromActiveSet( const Component& Component, std::vector<uint8_t>& ActiveSet )
{
    for ( int32_t const Tid : Component.Indices )
    {
        ActiveSet[Tid] = static_cast<uint8_t>( ProcessingState::Invalid );
    }
}

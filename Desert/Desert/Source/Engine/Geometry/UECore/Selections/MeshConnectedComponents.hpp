// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Selections/MeshConnectedComponents.h:18-165,
// 190-197, adapted: UE Core via UECore.hpp, namespace Desert::Geometry, TIndirectArray<FComponent> is a
// TArray<FComponent> (components are appended whole, so no reference into the array is held across an Add). Only
// the triangle seed-list path FGroupEdgeInserter uses (GroupEdgeInserter.cpp:764, :1084) is ported:
// FindTrianglesConnectedToSeeds with and without a connectivity predicate. The vertex, ROI and filter variants,
// SortByCount, the Initialize* and GrowTo* helpers have no caller here and are not carried.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

namespace Desert::Geometry
{
    /**
     * Connected components of a mesh, grown from seed triangles. Mesh connectivity is used unless a
     * predicate says when two neighbouring triangles count as connected.
     */
    class FMeshConnectedComponents
    {
    public:
        const FDynamicMesh3* Mesh;

        struct FComponent
        {
            /** Triangle IDs in the component, in the order they were reached. */
            TArray<int> Indices;
        };

        TArray<FComponent> Components;

        explicit FMeshConnectedComponents( const FDynamicMesh3* MeshIn ) : Mesh( MeshIn )
        {
        }

        [[nodiscard]] int32_t Num() const
        {
            return Components.Num();
        }
        [[nodiscard]] const FComponent& GetComponent( int32_t Index ) const
        {
            return Components[Index];
        }
        FComponent& GetComponent( int32_t Index )
        {
            return Components[Index];
        }
        const FComponent& operator[]( int32_t Index ) const
        {
            return Components[Index];
        }
        FComponent& operator[]( int32_t Index )
        {
            return Components[Index];
        }

        /**
         * One component per seed that an earlier seed's component has not already absorbed. A seed that is not
         * a live triangle starts nothing. TrisConnectedPredicate(t0, t1), when set, must also hold for t1 to be
         * reached from its neighbour t0.
         */
        void FindTrianglesConnectedToSeeds(
             const TArray<int>&                             SeedTriangles,
             const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate = nullptr );

    protected:
        void FindTriComponents( const TArray<int32_t>& SeedList, TArray<uint8_t>& ActiveSet,
                                const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate );
        void FindTriComponent( FComponent& Component, TArray<int32_t>& ComponentQueue,
                               TArray<uint8_t>& ActiveSet ) const;
        void FindTriComponent( FComponent& Component, TArray<int32_t>& ComponentQueue, TArray<uint8_t>& ActiveSet,
                               const std::function<bool( int32_t, int32_t )>& TriConnectedPredicate ) const;
        static void RemoveFromActiveSet( const FComponent& Component, TArray<uint8_t>& ActiveSet );
    };
} // namespace Desert::Geometry

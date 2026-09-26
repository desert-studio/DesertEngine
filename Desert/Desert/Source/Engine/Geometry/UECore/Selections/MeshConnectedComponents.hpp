// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Selections/MeshConnectedComponents.h:18-165,
// 190-197, adapted: UE Core via UECore.hpp, namespace Desert::Geometry, TIndirectArray<Component> is a
// TArray<Component> (components are appended whole, so no reference into the array is held across an Add). Only
// the triangle seed-list path GroupEdgeInserter uses (GroupEdgeInserter.cpp:764, :1084) is ported:
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
    class MeshConnectedComponents
    {
    public:
        const DynamicMesh3* m_Mesh;

        struct Component
        {
            /** Triangle IDs in the component, in the order they were reached. */
            std::vector<int> Indices;
        };

        std::vector<Component> m_Components;

        explicit MeshConnectedComponents( const DynamicMesh3* MeshIn ) : m_Mesh( MeshIn )
        {
        }

        [[nodiscard]] int32_t Num() const
        {
            return static_cast<int32_t>( m_Components.size() );
        }
        [[nodiscard]] const Component& GetComponent( int32_t Index ) const
        {
            return m_Components[Index];
        }
        Component& GetComponent( int32_t Index )
        {
            return m_Components[Index];
        }
        const Component& operator[]( int32_t Index ) const
        {
            return m_Components[Index];
        }
        Component& operator[]( int32_t Index )
        {
            return m_Components[Index];
        }

        /**
         * One component per seed that an earlier seed's component has not already absorbed. A seed that is not
         * a live triangle starts nothing. TrisConnectedPredicate(t0, t1), when set, must also hold for t1 to be
         * reached from its neighbour t0.
         */
        void FindTrianglesConnectedToSeeds(
             const std::vector<int>&                        SeedTriangles,
             const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate = nullptr );

    protected:
        void        FindTriComponents( const std::vector<int32_t>& SeedList, std::vector<uint8_t>& ActiveSet,
                                       const std::function<bool( int32_t, int32_t )>& TrisConnectedPredicate );
        void        FindTriComponent( Component& Component, std::vector<int32_t>& ComponentQueue,
                                      std::vector<uint8_t>& ActiveSet ) const;
        void        FindTriComponent( Component& Component, std::vector<int32_t>& ComponentQueue,
                                      std::vector<uint8_t>&                          ActiveSet,
                                      const std::function<bool( int32_t, int32_t )>& TriConnectedPredicate ) const;
        static void RemoveFromActiveSet( const Component& Component, std::vector<uint8_t>& ActiveSet );
    };
} // namespace Desert::Geometry
